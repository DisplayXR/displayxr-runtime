// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native Vulkan compositor implementation.
 * @author David Fattal
 * @ingroup comp_vk_native
 *
 * Follows the D3D11 native compositor pattern: direct Vulkan rendering +
 * display processor, no multi-compositor involvement. Uses the app's
 * VkDevice directly via vk_bundle (Monado's Vulkan wrapper).
 */

#include "comp_vk_native_compositor.h"
#include "comp_vk_native_deposit.h"
#include "comp_vk_native_swapchain.h"
#include "comp_vk_native_target.h"
#include "comp_vk_native_renderer.h"

#include "util/comp_layer_accum.h"
#include "util/comp_bg2d.h"
#ifdef XRT_OS_WINDOWS
#include "util/comp_display_refresh_win.h"
#endif

#include "xrt/xrt_handles.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_vulkan_includes.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_display_processor.h"
#include "xrt/xrt_display_processor_vk.h"

#include "vk/vk_helpers.h"
#include "vk/vk_hud_blend.h"
#include "vk/vk_local2d_composite.h"

#include "util/u_logging.h"
#include "util/u_debug.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_hud.h"
#include "os/os_time.h"
#include "os/os_threading.h"

#include "math/m_api.h"
#include "util/u_tiling.h"
#include "util/u_canvas.h"
#include "util/u_capture_intent.h"
#include "util/u_image_capture.h"
#include <displayxr_mcp/mcp_capture.h>

// STB_IMAGE_WRITE_STATIC scopes all stbi_write_* to this TU so linking
// alongside other compositors that also implement stb doesn't produce
// duplicate symbols.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "d3d11/comp_d3d11_window.h"
// #918 Phase 3 — the shared `weave placement:` line, plus the canonical reason
// tokens. Header-only from here: this compositor has NO output-device split (see
// the placement block in comp_vk_native_compositor_create), so it never links
// comp_xbridge, it only needs to name itself the same way the split paths do.
#include "d3d/d3d_weave_placement.h"
#include "comp_split_gate.h"
#endif

#ifdef XRT_OS_MACOS
#include "vk_native/comp_vk_native_window_macos.h"
#include <IOSurface/IOSurface.h>
#endif

// Desktop Linux (X11/XCB present path). Android also defines XRT_OS_LINUX, so the
// XCB code must be gated on "Linux AND NOT Android" — Android uses the
// VK_KHR_android_surface path, not XCB. See docs/roadmap/linux-support.md.
#if defined(XRT_OS_LINUX) && !defined(XRT_OS_ANDROID)
#define XRT_OS_LINUX_DESKTOP
#endif

#ifdef XRT_OS_LINUX_DESKTOP
#include "vk_native/comp_vk_native_window_xcb.h"
#endif

#ifdef XRT_OS_ANDROID
// #1037/#1033: the app publishes its window's on-screen rect through
// android_globals (XR_DXR_android_surface_binding); we consume it for both the
// DP weave phase and the per-window Kooima canvas.
#include "android/android_globals.h"
#endif

// Direct-scanout present path (ST-5539). Only compiled when the bundle carries
// the display + acquire-xlib platform (CMake: XRT_HAVE_XLIB_XRANDR); the whole
// direct branch below is gated on the same macro so its symbols exist.
#if defined(VK_USE_PLATFORM_XLIB_XRANDR_EXT) && defined(VK_USE_PLATFORM_DISPLAY_KHR)
#define DXR_HAVE_DIRECT_SCANOUT
#include "vk_native/comp_vk_native_window_direct.h"
#endif

// Wayland windowed weaving (#817): the window's absolute position comes from
// the compositor-published session-bus geometry service, since Wayland never
// tells a client where its surface sits. D-Bus is optional; without it the
// Wayland path weaves display-scoped as before.
#if defined(XRT_HAVE_WAYLAND) && defined(XRT_HAVE_DBUS)
#define DXR_HAVE_WL_GEOM
#include "vk_native/comp_vk_native_wl_geom.h"
#endif

#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef XRT_OS_WINDOWS
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// Decoupled presentation (#833): same env the targets read. On a transparent
// session the DP's alpha-gate flattens against the captured desktop (plugin
// #116), so the post-weave Local2D composite must flatten too instead of
// emitting DWM-dependent alpha.
DEBUG_GET_ONCE_BOOL_OPTION(present_opaque, "DXR_PRESENT_OPAQUE", false)
// #862 — clip the Local2D composite to the pixels where it is not the
// identity. On by default; DXR_L2D_CLIP=0 restores the full-region pass for
// A/B measurement (the win is GPU fill, so compare GPU busy time, not the
// frame-stage timer — that stage also spans the submit + whole-frame wait).
DEBUG_GET_ONCE_BOOL_OPTION(local2d_clip, "DXR_L2D_CLIP", true)

// Frame pipelining (#837): per-stage CPU timing of the windowed layer_commit
// path — where does the non-GPU-busy frame time go? Accumulates per stage and
// logs a one-line summary ~1/sec (WARN so it survives the hot-path filter).
DEBUG_GET_ONCE_BOOL_OPTION(frame_stage_timing, "DXR_FRAME_STAGE_TIMING", false)

enum vk_frame_stage
{
	VK_FSTAGE_PRE = 0,      // commit entry → pre-DP cmd recorded (renderer, accum, crops)
	VK_FSTAGE_PREFLUSH,     // pre-DP submit + queue drain (self-submitting DP flush)
	VK_FSTAGE_WEAVE,        // xrt_display_processor_process_atlas (SDK-internal submit+wait)
	VK_FSTAGE_POSTWAIT,     // post-weave vkDeviceWaitIdle (audit B7)
	VK_FSTAGE_COMPOSITE,    // Local2D composite + HUD record + submit + drain
	VK_FSTAGE_PRESENT,      // comp_vk_native_target_present (bridge copy + DXGI Present)
	VK_FSTAGE_COUNT
};

struct vk_frame_timing
{
	uint64_t stage_ns[VK_FSTAGE_COUNT];
	uint32_t frames;
	uint64_t last_log_ns;
};

static void
vk_frame_timing_add(struct vk_frame_timing *t, enum vk_frame_stage s, uint64_t t0, uint64_t t1)
{
	t->stage_ns[s] += t1 - t0;
}

static void
vk_frame_timing_flush(struct vk_frame_timing *t, uint64_t now)
{
	t->frames++;
	if (t->last_log_ns == 0) {
		t->last_log_ns = now;
		return;
	}
	if (now - t->last_log_ns < 1000000000ull || t->frames == 0) {
		return;
	}
	const double per = 1.0e-6 / (double)t->frames; // ns → ms per frame
	// NB the label: this stage spans the composite + HUD RECORDING *and* the
	// submit and whole-frame GPU drain, so at a paced frame rate it mostly
	// measures the wait, not the composite. Reading it as "composite cost"
	// invites optimising the wrong thing (#862) — name it for what it is.
	U_LOG_W("[FRAME_STAGES] n=%u pre=%.2f preflush=%.2f weave=%.2f postwait=%.2f composite+wait=%.2f "
	        "present=%.2f (ms/frame)",
	        t->frames, t->stage_ns[VK_FSTAGE_PRE] * per, t->stage_ns[VK_FSTAGE_PREFLUSH] * per,
	        t->stage_ns[VK_FSTAGE_WEAVE] * per, t->stage_ns[VK_FSTAGE_POSTWAIT] * per,
	        t->stage_ns[VK_FSTAGE_COMPOSITE] * per, t->stage_ns[VK_FSTAGE_PRESENT] * per);
	memset(t->stage_ns, 0, sizeof(t->stage_ns));
	t->frames = 0;
	t->last_log_ns = now;
}

/*!
 * Minimal settings struct for Vulkan compositor.
 */
struct comp_vk_settings
{
	struct
	{
		uint32_t width;
		uint32_t height;
	} preferred;

	int64_t nominal_frame_interval_ns;
};

/*!
 * The Vulkan native compositor structure.
 */
struct comp_vk_native_compositor
{
	//! Base type - must be first!
	struct xrt_compositor_native base;

	//! The device we are rendering for.
	struct xrt_device *xdev;

	//! Vulkan bundle (initialized from app's VkDevice via vk_init_from_given).
	struct vk_bundle vk;

	//! Queue family index.
	uint32_t queue_family_index;

	//! Output target (VkSwapchainKHR).
	struct comp_vk_native_target *target;

	//! Renderer for layer compositing.
	struct comp_vk_native_renderer *renderer;

	//! VK-0 (#1178): the app's VkDevice has VK_KHR_timeline_semaphore enabled.
	//! Read only by the deposit, which cannot exist without it.
	bool app_timeline_semaphores;

	//! Accumulated layers for the current frame.
	struct comp_layer_accum layer_accum;

	//! Compositor settings.
	struct comp_vk_settings settings;

#ifdef XRT_OS_WINDOWS
	//! Window handle (either from app or self-created).
	void *hwnd;

	//! Self-created window (NULL if app provided window).
	struct comp_d3d11_window *own_window;

	//! True if we created the window ourselves.
	bool owns_window;
#endif

#ifdef XRT_OS_MACOS
	//! macOS window helper (self-owned or external view).
	struct comp_vk_native_window_macos *macos_window;

	//! True if we created the window ourselves.
	bool owns_window;
#endif

#ifdef XRT_OS_LINUX_DESKTOP
	//! XCB window helper (self-owned, hosted class). NULL when the app
	//! supplied its own window via XR_DXR_xlib_window_binding (handle class).
	struct comp_vk_native_window_xcb *xcb_window;

	//! Connection + window id handed to the target as the type-erased hwnd.
	//! Stored here (outlives the target) so the surface's borrowed connection
	//! stays valid for the target's lifetime.
	struct comp_vk_native_xcb_handle xcb_handle;

	//! True if we created the window ourselves.
	bool owns_window;

#ifdef DXR_HAVE_DIRECT_SCANOUT
	//! Direct-scanout backend (owns the acquired connector + display-plane
	//! surface). Non-NULL only when DXR_LINUX_DIRECT_SCANOUT opted in AND the
	//! acquire succeeded; NULL means we stayed on the XCB path. ST-5539.
	struct comp_vk_native_window_direct *direct_window;
#endif
#ifdef XRT_HAVE_WAYLAND
	//! True when the app supplied a Wayland surface (XR_DXR_wayland_surface_binding)
	//! instead of an X11 window — the target builds a VkWaylandSurfaceKHR.
	bool use_wayland;
	//! wl_display* + wl_surface* handed to the target as the type-erased hwnd.
	struct comp_vk_native_wayland_handle wayland_handle;
#endif
#ifdef DXR_HAVE_WL_GEOM
	//! Compositor-side window-geometry provider (#817). NULL when the session
	//! bus is unreachable; the provider itself degrades to "no data" when the
	//! GNOME Shell extension is absent.
	struct comp_vk_native_wl_geom *wl_geom;
#endif
#endif

	//! Shared texture VkImage (imported from HANDLE).
	VkImage shared_image;

	//! Shared texture memory.
	VkDeviceMemory shared_memory;

	//! Shared texture image view.
	VkImageView shared_view;

	//! True if shared texture mode is active.
	bool has_shared_texture;

	//! Shared texture HANDLE (Win32).
	void *shared_texture_handle;

	//! Command pool for display processor factory.
	VkCommandPool cmd_pool;

	//! Generic Vulkan display processor (vendor-agnostic weaving).
	struct xrt_display_processor *display_processor;

	//! Transparent-background present requested at create (XR_EXT_*_window_binding
	//! transparentBackgroundEnabled). The atlas/DP clear to alpha=0 and the
	//! present uses a transparent compositeAlpha. Cached for the macOS Local2D
	//! flat-2D-over-desktop rule (#568) in vk_composite_local_2d.
	bool transparent_background;

	//! Compose-under backdrop for the base-DP slot-16 seam (#1073). The
	//! out-of-process path produces this in comp_multi_system.c; in-process
	//! this is the same producer, so an Arch-A app (its own translucent
	//! window, no service overlay, no anti-tapjacking alpha clamp) gets the
	//! same opaque-band weave. Only ever populated when nothing else already
	//! claimed the slot — see vk_bg2d_backdrop.
	struct comp_bg2d_state bg2d;

	//! True when display is in 3D mode (weaver active). False = 2D passthrough.
	bool hardware_display_3d;

	//! Per-frame effective CONTENT layout (#542): the atlas grid actually
	//! painted and handed to the DP this frame — submission-derived,
	//! decoupled from hardware_display_3d. views == 0 until the first
	//! layer commit computes it.
	struct comp_vk_native_eff_layout eff_layout;

	//! Last known 3D rendering mode index (for V-key toggle restore).
	uint32_t last_3d_mode_index;

	//! True if app is legacy (no XR_DXR_display_info) — gates 1/2/3 key mode selection.
	bool legacy_app_tile_scaling;

	//! System devices (for qwerty driver).
	struct xrt_system_devices *xsysd;

	//! Current frame ID.
	int64_t frame_id;

	//! Display refresh rate in Hz.
	float display_refresh_rate;

	//! Last present origin handed to the DP (panel-relative px) — an origin
	//! change means the window is being dragged; the target clamps its queue
	//! shallow for the duration so the weave phase stays snapped (#912).
	int last_present_origin_x;
	int last_present_origin_y;
	bool have_last_present_origin;

#ifdef XRT_OS_ANDROID
	//! Last window-rect generation (android_globals) already handed to the DP
	//! / already logged, so the per-window rect feed and its Kooima line stay
	//! lifecycle events rather than per-frame noise (#1037).
	uint64_t android_rect_generation_fed;
	uint64_t android_rect_generation_logged;
#endif

	/*!
	 * #602: the target image-set generation the display processor has already
	 * been told about.
	 *
	 * begin_frame notifies when IT recreates the target, but that is not the
	 * only recreate path: comp_vk_native_target_acquire rebuilds the swapchain
	 * itself on VK_ERROR_OUT_OF_DATE_KHR, and nothing told the DP. It then
	 * kept a strip-framebuffer cache keyed by VkImage handles that no longer
	 * exist — Vulkan recycles freed handles, so the cache silently aliases
	 * destroyed images and the next use faults the device
	 * (VK_ERROR_DEVICE_LOST from vkQueueSubmit). Comparing against this makes
	 * the notification happen for EVERY recreate, whoever caused it.
	 */
	uint32_t dp_notified_target_generation;

	/*!
	 * #868: serialises the frame path against the repaint replay.
	 *
	 * VK had no compositor lock at all. It needs one for the same reason D3D11
	 * did, plus a stronger one: `vk->main_queue->queue` is the APP's queue, and
	 * a VkQueue requires EXTERNAL SYNCHRONISATION — two threads calling
	 * vkQueueSubmit/vkQueuePresentKHR on it is undefined behaviour, not merely
	 * a lost render target. The vendor weave records into our command buffer
	 * (setCommandBuffer + weave()), but its texture-reload path submits and
	 * waits on that same queue from inside weave(), and which weaver binary is
	 * loaded varies by installed SR version. Holding this across the whole
	 * replay means the queue and the display processor only ever see one
	 * caller, whichever weaver is in play.
	 */
	struct os_mutex mutex;

	//! #868: the repaint loop. See vk_repaint_thread.
	struct os_thread_helper repaint_thread;

	/*!
	 * #868: a VkQueue the RUNTIME owns exclusively, used ONLY by the repaint
	 * replay. VK_NULL_HANDLE when none was obtainable, in which case the
	 * repaint stays disabled.
	 *
	 * The app's queue cannot be used: a VkQueue is externally synchronised and
	 * the APP submits to it on its own thread, outside any lock the runtime
	 * controls. Sharing it produced VK_ERROR_DEVICE_LOST and validation's
	 * "UNASSIGNED-Threading-MultipleThreads-Write". Requested at device
	 * creation under vulkan_enable2 — see oxr_vk_create_vulkan_device.
	 */
	VkQueue repaint_queue;
	int32_t repaint_queue_family;
	int32_t repaint_queue_index;

	/*!
	 * #868: fence and command pool used ONLY by the repaint replay.
	 *
	 * frame_fence and the renderer's command pool cannot be shared once the
	 * replay runs on its own queue. With a single queue the app frame's fence
	 * wait drained everything before a repaint could start; with two queues
	 * nothing does, and validation reports the whole "still in use" family:
	 * vkQueueSubmit-fence-00064 (fence already in use),
	 * vkResetFences-pFences-01123 (reset while in flight),
	 * vkBeginCommandBuffer-commandBuffer-00049 and
	 * vkQueueSubmit-pCommandBuffers-00071 (buffer re-recorded / resubmitted
	 * while still pending). Separate objects remove the sharing entirely.
	 */
	VkFence repaint_fence;
	VkCommandPool repaint_cmd_pool;

	/*!
	 * #868: everything the repaint thread needs to replay the last app frame's
	 * weave WITHOUT touching app-owned state. Published by layer_commit.
	 */
	struct
	{
		int enabled;                    //!< DXR_WEAVE_REPAINT=0 disables.
		int force;                      //!< DXR_WEAVE_REPAINT_FORCE=1 correctness probe.

		/*!
		 * #902: shared-queue tier — no runtime-owned queue exists (single
		 * graphics-queue GPU), but VK_LAYER_DXR_queue_lock is live in the
		 * device chain (marker resolved), so the repaint submits on the APP's
		 * queue and the layer serializes every vkQueue* call per-queue. Set
		 * only after the marker handshake; never assumed.
		 */
		int shared_queue;
		bool armed;                     //!< False on zero-copy: the atlas IS the app's image.
		bool app_frame_in_progress;     //!< Set by layer_begin, cleared by layer_commit.
		uint64_t last_app_frame_ns;     //!< Quiet-gate key. Never touched by a repaint.

		//! Effective content layout the last app frame actually painted.
		uint32_t view_w, view_h, cols, rows;

		//! Compositor-owned atlas the last app frame wove from.
		uint64_t atlas_image, atlas_view;
		int32_t atlas_format;
		uint32_t atlas_w, atlas_h;

		/*!
		 * The 2D-under backdrop the last app frame FLATTENED. Cached, never
		 * re-flattened: the flatten samples the app's own Local2D swapchain
		 * images, which it has since reacquired and overwritten. Re-reading
		 * them is what made the D3D11/D3D12 bubble flicker.
		 */
		uint64_t backdrop_view;
		uint32_t backdrop_w, backdrop_h;

		//! The zone/Local2D mask the last app frame RESOLVED. Replayed as-is —
		//! a repaint neither re-rasterises it nor re-publishes the wish.
		uint64_t mask_view;

		uint64_t count, ticks;          //!< Diagnostics.
	} repaint;

	//! Time of the last predicted display time.
	uint64_t last_display_time_ns;

	//! System compositor info (display dimensions, nominal viewer position).
	bool sys_info_set;
	struct xrt_system_compositor_info sys_info;

	//! HUD overlay (shared u_hud system).
	struct u_hud *hud;

	//! HUD texture (VkImage, CPU-uploadable).
	VkImage hud_image;
	VkDeviceMemory hud_memory;
	uint32_t hud_width;
	uint32_t hud_height;

	//! Smoothed frame time for HUD FPS display.
	float smoothed_frame_time_ms;

	//! Last frame timestamp for dt calculation.
	uint64_t last_frame_ns;

	//! Lazily allocated intermediate image for cropping atlas to content dims.
	VkImage dp_input_image;
	VkImageView dp_input_view;
	VkDeviceMemory dp_input_memory;
	uint32_t dp_input_width, dp_input_height;


	//! Alpha-blend pipeline for window-space (HUD) layers rendered per-tile
	//! INTO the atlas pre-weave (matches d3d11/d3d12/metal/gl). Lazy-init
	//! with the atlas format on first window-space submission. The DP weaver
	//! then interlaces the layer along with the projection content, giving
	//! HUD per-eye disparity / parallax (#210).
	struct vk_hud_blend window_space_blend;
	bool window_space_blend_attempted;
	//! Cached framebuffer for atlas window-space pass (one per atlas view).
	VkFramebuffer atlas_ws_fb;
	VkImageView atlas_ws_fb_view;

	//! MCP capture_frame request box (serviced at end of layer_commit).
	//! Mirrors the pattern in comp_metal/gl/d3d11_compositor. See issue #210.
	struct mcp_capture_request mcp_capture;

	//! Per-frame capture intent (mode + path), populated by
	//! u_capture_intent_poll at the top of layer_commit and consumed
	//! at the projection-done boundary (PROJECTION_ONLY) or end of
	//! frame (POST_COMPOSE).
	struct u_capture_intent capture_intent;

	//! Composite-tap diagnostics (#833 debugging): the Local2D composite
	//! stashes this frame's target + scratch state here so the
	//! `displayxr_composite_tap_trigger` file (in %TEMP%) can dump the
	//! final target, the weave snapshot (post-DP/gate) and the flattened
	//! 2D scratch as PNGs at end of layer_commit. Zeroed at the top of
	//! each commit; only valid on frames where the composite ran.
	VkImage tap_target_image;
	VkImageLayout tap_target_layout;
	uint32_t tap_target_w, tap_target_h;
	uint32_t tap_region_w, tap_region_h;

	//! #837 — fence for the windowed path's per-frame submit. Waiting this
	//! instead of vkQueueWaitIdle scopes the CPU stall to OUR submission
	//! (identical on a single-queue iGPU, strictly narrower elsewhere) and
	//! is the stepping stone for deferred present. Lazily created.
	VkFence frame_fence;

	//! #439 Phase 3 — masked 2D-over-3D composite (post-weave). Pipelines +
	//! render passes; created eagerly at compositor init (formats are fixed
	//! B8G8R8A8_UNORM for both the target and the scratch — see the init).
	struct vk_local2d_composite local2d;
	bool local2d_initialized;

	//! twod flatten scratch (B8G8R8A8_UNORM, COLOR_ATTACHMENT|SAMPLED). The
	//! frame's OVER Local2D layers (after the projection in list order) are
	//! flattened here, then sampled as `twod` by the masked composite. Lazily
	//! (re)allocated to the window region dims.
	VkImage local2d_scratch;
	VkDeviceMemory local2d_scratch_mem;
	VkImageView local2d_scratch_view;
	VkFramebuffer local2d_scratch_fb;
	uint32_t local2d_scratch_w, local2d_scratch_h;

	//! #491 part 3 — 2D-under backdrop scratch (same fmt/usage as
	//! local2d_scratch). The frame's UNDER Local2D layers (before the projection
	//! in list order) are flattened here PRE-weave and handed to the DP via
	//! set_background_2d, so the DP composites `backdrop over captured-desktop`
	//! as the under-3D background. Compositor-owned so it outlives process_atlas
	//! (the DP samples it). Left in SHADER_READ_ONLY_OPTIMAL after the flatten.
	VkImage backdrop_scratch;
	VkDeviceMemory backdrop_scratch_mem;
	VkImageView backdrop_scratch_view;
	VkFramebuffer backdrop_scratch_fb;
	uint32_t backdrop_scratch_w, backdrop_scratch_h;

	//! Weave snapshot scratch (target format, TRANSFER_DST|SAMPLED). The DP
	//! wrote the woven 3D into the target (RT≠SRV), so the lerp reads this
	//! copy. Lazily (re)allocated to the window region dims.
	VkImage weave_scratch;
	VkDeviceMemory weave_scratch_mem;
	VkImageView weave_scratch_view;
	uint32_t weave_scratch_w, weave_scratch_h;

	//! Runtime-owned IMPLICIT zone mask (R8_UNORM, COLOR_ATTACHMENT|SAMPLED),
	//! rasterized each frame from the Local2D layer rects (M=1 keep weave, M=0
	//! inside the rects). Sampled by the composite when no explicit mask is
	//! submitted. No staged sibling needed — raster + sample share one cmd
	//! buffer with a barrier between (unlike the cross-call explicit mask).
	VkImage implicit_mask_tex;
	VkDeviceMemory implicit_mask_mem;
	VkImageView implicit_mask_view;
	VkFramebuffer implicit_mask_fb;
	uint32_t implicit_mask_w, implicit_mask_h;

	//! Zones COMPOSITE mask with per-zone opt-in feather (runtime#800).
	//! Allocated only when a frame's zones request feather — the published
	//! wish must stay binary (implicit_mask above), so a feathered composite
	//! needs its own image. All-hard frames reuse implicit_mask for both.
	VkImage feather_mask_tex;
	VkDeviceMemory feather_mask_mem;
	VkImageView feather_mask_view;
	VkFramebuffer feather_mask_fb;
	uint32_t feather_mask_w, feather_mask_h;

	//! Composite-target framebuffer cache (over the rotating swapchain/shared
	//! image view, keyed by view — the DP target image rotates per frame).
	VkFramebuffer composite_target_fb;
	VkImageView composite_target_fb_view;
	uint32_t composite_fb_w, composite_fb_h;

	//! Active authored zone mask (#439 Phase 1, XR_DXR_local_3d_zone). Sticky
	//! last-submit-wins; cleared on destroy. NOT owned (the oxr handle owns it).
	struct comp_vk_native_zone_mask *active_zone_mask;

	//! True when the last committed frame carried Local2D layers — makes the
	//! implicit mask's canvas-supersede visible to vk_effective_canvas. Set once
	//! under the frame path at the top of layer_commit.
	bool local_2d_last_frame;

	//! XR_DXR_display_zones (ADR-027): true when the current frame's
	//! accumulator carries XRT_LAYER_ZONE_3D layers (a "zones frame"). The
	//! effective canvas is the full client window; the wish drives the
	//! post-weave lerp (the implicit-mask + sticky-mask rules are inert).
	//! P4: the hardware leg is the per-frame wish publish below for
	//! zone-capable DPs; legacy DPs keep the tier-1 global fallback.
	bool zones_frame;

	//! Explicit per-frame wish (XrDisplayZonesFrameEndInfoDXR.wishMask) set
	//! via comp_vk_native_compositor_zones_set_frame_wish before commit;
	//! NULL = auto-derive from the zone rects. Not owned.
	struct comp_vk_native_zone_mask *frame_wish;

	//! Tier-1 fallback edge state: request_display_mode(true) fired once on
	//! the zones rising edge ("any zone active => request 3D"); never forces
	//! 2D on the falling edge (mode restore stays with the V-toggle logic).
	//! P4: only taken for legacy DPs (caps.supported == 0) — a zone-capable
	//! DP gets the per-frame wish publish instead (vk_sync_zone_mask_to_dp).
	bool zones_mode_requested;

	//! Frame's effective atlas-capture source — the image the DP actually
	//! received this frame: the renderer atlas for normal frames, or the app's
	//! zero-copy swapchain when the submission exactly filled the worst-case
	//! atlas (full-fill modes like Quad). Set each frame in layer_commit so the
	//! atlas capture reflects what the DP got, independent of render mode —
	//! otherwise a zero-copy frame captures the (unpainted) renderer atlas =
	//! black. SHADER_READ_ONLY_OPTIMAL at the capture point (the DP sampled it).
	uint64_t capture_src_image_u64;
	int32_t capture_src_format;
	uint32_t capture_src_atlas_w, capture_src_atlas_h;

	//! #224 / ADR-027 hardware-DP zone leg (P4): cached get_local_zone_caps
	//! result. 0 = not queried yet (calloc default), 1 = supported,
	//! 2 = legacy DP (slot absent / caps unsupported — never publish).
	int zone_dp_state;
	//! DP zone caps when zone_dp_state == 1.
	struct xrt_dp_local_zone_caps zone_dp_caps;
	//! Published-content generation: bumped on zone_mask_submit (legacy
	//! sticky mask), on an auto-wish re-raster whose rect set / dims
	//! actually changed, and on an explicit-frame-wish source change —
	//! NOT per frame (VK re-rasters the wish in-cmd every zones frame;
	//! identical content keeps its generation so vendors evaluate once).
	uint64_t zone_publish_seq;
	//! True while this client's mask is published to the DP — drives the
	//! clear-on-deactivate edge.
	bool zone_published;
	//! This frame's resolved wish view + dims, set by vk_composite_local_2d
	//! in zones frames (explicit frame wish staged view, or the auto raster
	//! reusing implicit_mask_view) and reset at the top of layer_commit.
	//! The publish runs AFTER the frame's queue submit + wait-idle, so the
	//! view content is GPU-complete and in SHADER_READ_ONLY_OPTIMAL —
	//! exactly the publish contract. Stays VK_NULL_HANDLE on paths that
	//! never run the composite (e.g. shared-image zones frames).
	VkImageView zone_wish_view;
	uint32_t zone_wish_w, zone_wish_h;
	//! Seq-bump caches: last explicit wish pointer actually published, and
	//! the auto raster's rect set + dims (mirrors d3d11's wish_rects).
	struct comp_vk_native_zone_mask *zone_frame_wish_last;
	struct xrt_rect zone_wish_rects[XRT_MAX_LAYERS];
	uint32_t zone_wish_rect_count;

	//! #491 part 3 — guards vk_local2d_composite_begin_frame (which resets the
	//! shared descriptor pool) to run at most once per frame. Both the pre-weave
	//! backdrop flatten and the post-weave overlay composite allocate from the
	//! one pool; resetting it twice while a shared cmd buffer still references
	//! the earlier sets is a use-after-reset. Cleared at the top of layer_commit.
	bool local2d_pool_reset_this_frame;
};

/*
 *
 * Helper functions
 *
 */

#ifdef XRT_OS_MACOS
/*!
 * Import an IOSurface as a VkImage for shared texture rendering.
 *
 * Uses VK_EXT_metal_objects to import the IOSurface, export the MTLTexture,
 * and allocate memory via VK_EXT_external_memory_metal.
 */
static bool
import_shared_iosurface(struct comp_vk_native_compositor *c, void *iosurface_handle)
{
	struct vk_bundle *vk = &c->vk;

#if defined(VK_EXT_metal_objects) && defined(VK_EXT_external_memory_metal)
	if (!vk->has_EXT_metal_objects || !vk->has_EXT_external_memory_metal) {
		U_LOG_E("VK_EXT_metal_objects or VK_EXT_external_memory_metal not available");
		return false;
	}

	IOSurfaceRef surface = (IOSurfaceRef)iosurface_handle;
	uint32_t width = (uint32_t)IOSurfaceGetWidth(surface);
	uint32_t height = (uint32_t)IOSurfaceGetHeight(surface);

	if (width == 0 || height == 0) {
		U_LOG_E("IOSurface has zero dimensions");
		return false;
	}

	U_LOG_W("Importing IOSurface %ux%u as VkImage for shared texture", width, height);

	// Chain: VkImageCreateInfo -> VkImportMetalIOSurfaceInfoEXT -> VkExportMetalObjectCreateInfoEXT
	VkExportMetalObjectCreateInfoEXT export_metal_tex_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
	    .pNext = NULL,
	    .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT,
	};
	VkImportMetalIOSurfaceInfoEXT import_iosurface_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_METAL_IO_SURFACE_INFO_EXT,
	    .pNext = &export_metal_tex_info,
	    .ioSurface = surface,
	};

	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &import_iosurface_info,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,  // IOSurface is BGRA8
	    .extent = {width, height, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	             VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkResult ret = vk->vkCreateImage(vk->device, &image_ci, NULL, &c->shared_image);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkCreateImage for shared IOSurface failed: %d", ret);
		return false;
	}

	// Export MTLTexture from the VkImage (MoltenVK created it from the IOSurface)
	VkExportMetalTextureInfoEXT export_tex_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
	    .image = c->shared_image,
	    .imageView = VK_NULL_HANDLE,
	    .bufferView = VK_NULL_HANDLE,
	    .plane = VK_IMAGE_ASPECT_COLOR_BIT,
	    .mtlTexture = NULL,
	};
	VkExportMetalObjectsInfoEXT export_objects_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
	    .pNext = &export_tex_info,
	};
	vk->vkExportMetalObjectsEXT(vk->device, &export_objects_info);

	void *metal_texture_handle = (void *)export_tex_info.mtlTexture;
	if (metal_texture_handle == NULL) {
		U_LOG_E("Failed to export MTLTexture from shared VkImage");
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		return false;
	}

	// Get memory requirements
	VkMemoryRequirements requirements = {0};
	vk->vkGetImageMemoryRequirements(vk->device, c->shared_image, &requirements);

	// Get valid memory type bits from the MTLTexture
	VkMemoryMetalHandlePropertiesEXT metal_props = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_METAL_HANDLE_PROPERTIES_EXT,
	};
	ret = vk->vkGetMemoryMetalHandlePropertiesEXT(
	    vk->device,
	    VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT,
	    metal_texture_handle,
	    &metal_props);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkGetMemoryMetalHandlePropertiesEXT failed: %d", ret);
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		return false;
	}

	requirements.memoryTypeBits = metal_props.memoryTypeBits;

	// Import memory using the MTLTexture handle with dedicated allocation
	// (matches vk_helpers.c pattern for Metal texture import)
	VkImportMemoryMetalHandleInfoEXT import_memory_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_METAL_HANDLE_INFO_EXT,
	    .pNext = NULL,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT,
	    .handle = metal_texture_handle,
	};

	VkMemoryDedicatedAllocateInfoKHR dedicated_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
	    .pNext = &import_memory_info,
	    .image = c->shared_image,
	    .buffer = VK_NULL_HANDLE,
	};

	// Find a valid memory type
	VkPhysicalDeviceMemoryProperties mem_props;
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);

	uint32_t memory_type_index = UINT32_MAX;
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1u << i)) != 0) {
			memory_type_index = i;
			break;
		}
	}
	if (memory_type_index == UINT32_MAX) {
		U_LOG_E("No valid memory type for shared IOSurface");
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		return false;
	}

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &dedicated_info,
	    .allocationSize = requirements.size,
	    .memoryTypeIndex = memory_type_index,
	};

	ret = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &c->shared_memory);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkAllocateMemory for shared IOSurface failed: %d", ret);
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		return false;
	}

	ret = vk->vkBindImageMemory(vk->device, c->shared_image, c->shared_memory, 0);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkBindImageMemory for shared IOSurface failed: %d", ret);
		vk->vkFreeMemory(vk->device, c->shared_memory, NULL);
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		c->shared_memory = VK_NULL_HANDLE;
		return false;
	}

	// Create image view
	VkImageViewCreateInfo view_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = c->shared_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                   VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
	    .subresourceRange = {
	        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	        .baseMipLevel = 0,
	        .levelCount = 1,
	        .baseArrayLayer = 0,
	        .layerCount = 1,
	    },
	};

	ret = vk->vkCreateImageView(vk->device, &view_ci, NULL, &c->shared_view);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkCreateImageView for shared IOSurface failed: %d", ret);
		vk->vkFreeMemory(vk->device, c->shared_memory, NULL);
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		c->shared_memory = VK_NULL_HANDLE;
		return false;
	}

	c->has_shared_texture = true;
	c->settings.preferred.width = width;
	c->settings.preferred.height = height;

	U_LOG_W("Shared IOSurface imported: %ux%u, VkImage=%p, VkImageView=%p",
	        width, height, (void *)(uintptr_t)c->shared_image,
	        (void *)(uintptr_t)c->shared_view);
	return true;
#else
	U_LOG_E("VK_EXT_metal_objects not available at compile time");
	return false;
#endif
}
#endif // XRT_OS_MACOS

#ifdef XRT_OS_WINDOWS
/*!
 * Import a D3D11 shared texture HANDLE as a VkImage for shared texture rendering.
 *
 * Uses VK_KHR_external_memory_win32 to import the D3D11 MISC_SHARED handle
 * as VkDeviceMemory backed by the same GPU resource.
 */
static bool
import_shared_d3d11_texture(struct comp_vk_native_compositor *c, void *shared_handle)
{
	struct vk_bundle *vk = &c->vk;

	if (shared_handle == NULL) {
		U_LOG_E("shared_handle is NULL");
		return false;
	}

	// Size the VkImage to match the app's shared texture exactly. The app sizes
	// it to the display-native worst-case atlas (ADR-010 — display pixels), and
	// `settings.preferred` isn't populated yet at import time (the HWND/screen
	// sizing runs later in create), so reading it here gives 0 → a 1920x1080
	// guess that aliases only a sub-rect of the real 3840x2160 texture and weaves
	// the content squished into a corner (#613). Use the head device's native
	// screen dimensions — the same source the app's displayPixelWidth/Height
	// comes from — mirroring the macOS path that reads the actual IOSurface size.
	uint32_t width = 0, height = 0;
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		width = c->xdev->hmd->screens[0].w_pixels;
		height = c->xdev->hmd->screens[0].h_pixels;
	}
	if (width == 0 || height == 0) {
		width = 1920;
		height = 1080;
	}

	U_LOG_W("Importing D3D11 shared texture as VkImage: %ux%u, handle=%p",
	        width, height, shared_handle);

	// Create VkImage with external memory info for D3D11 KMT handle
	VkExternalMemoryImageCreateInfo external_ci = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	    .pNext = NULL,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT,
	};

	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &external_ci,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .extent = {width, height, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	             VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkResult ret = vk->vkCreateImage(vk->device, &image_ci, NULL, &c->shared_image);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkCreateImage for shared D3D11 texture failed: %d", ret);
		return false;
	}

	// Get memory requirements
	VkMemoryRequirements requirements = {0};
	vk->vkGetImageMemoryRequirements(vk->device, c->shared_image, &requirements);

	// Import D3D11 KMT handle as Vulkan memory
	VkImportMemoryWin32HandleInfoKHR import_memory_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
	    .pNext = NULL,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT,
	    .handle = (HANDLE)shared_handle,
	};

	VkMemoryDedicatedAllocateInfoKHR dedicated_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
	    .pNext = &import_memory_info,
	    .image = c->shared_image,
	    .buffer = VK_NULL_HANDLE,
	};

	// Find a valid memory type (device-local preferred)
	VkPhysicalDeviceMemoryProperties mem_props;
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);

	/*
	 * #879: an IMPORT must pick its memoryTypeIndex from
	 * VkMemoryWin32HandlePropertiesKHR::memoryTypeBits — the plain image
	 * requirements alone chose index 0 where the handle's legal set differed
	 * (VUID-VkMemoryAllocateInfo-memoryTypeIndex-00645). Both constraints
	 * apply, so intersect; fall back to the image bits if the proc is
	 * unavailable.
	 */
	uint32_t import_legal_bits = requirements.memoryTypeBits;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
	/*
	 * Resolved LOCALLY, not via vk_bundle: the bundle struct crosses the
	 * plug-in ABI boundary (the DP factory receives &c->vk), so adding a slot
	 * mid-struct is an ADR-020 break — and vk_init_from_given loads the
	 * external-memory procs conditionally anyway.
	 */
	PFN_vkGetMemoryWin32HandlePropertiesKHR get_handle_props =
	    (PFN_vkGetMemoryWin32HandlePropertiesKHR)vk->vkGetDeviceProcAddr(
	        vk->device, "vkGetMemoryWin32HandlePropertiesKHR");
	if (get_handle_props != NULL) {
		VkMemoryWin32HandlePropertiesKHR hp = {
		    .sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR,
		};
		if (get_handle_props(vk->device,
		                     VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT,
		                     (HANDLE)shared_handle, &hp) == VK_SUCCESS &&
		    hp.memoryTypeBits != 0) {
			import_legal_bits = hp.memoryTypeBits & requirements.memoryTypeBits;
			if (import_legal_bits == 0) {
				import_legal_bits = hp.memoryTypeBits;
			}
		}
	}
#endif

	uint32_t memory_type_index = UINT32_MAX;
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((import_legal_bits & (1u << i)) != 0) {
			memory_type_index = i;
			break;
		}
	}
	if (memory_type_index == UINT32_MAX) {
		U_LOG_E("No valid memory type for shared D3D11 texture");
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		return false;
	}

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &dedicated_info,
	    .allocationSize = requirements.size,
	    .memoryTypeIndex = memory_type_index,
	};

	ret = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &c->shared_memory);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkAllocateMemory for shared D3D11 texture failed: %d", ret);
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		return false;
	}

	ret = vk->vkBindImageMemory(vk->device, c->shared_image, c->shared_memory, 0);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkBindImageMemory for shared D3D11 texture failed: %d", ret);
		vk->vkFreeMemory(vk->device, c->shared_memory, NULL);
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		c->shared_memory = VK_NULL_HANDLE;
		return false;
	}

	// Create image view
	VkImageViewCreateInfo view_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = c->shared_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                   VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
	    .subresourceRange = {
	        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	        .baseMipLevel = 0,
	        .levelCount = 1,
	        .baseArrayLayer = 0,
	        .layerCount = 1,
	    },
	};

	ret = vk->vkCreateImageView(vk->device, &view_ci, NULL, &c->shared_view);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkCreateImageView for shared D3D11 texture failed: %d", ret);
		vk->vkFreeMemory(vk->device, c->shared_memory, NULL);
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		c->shared_memory = VK_NULL_HANDLE;
		return false;
	}

	c->has_shared_texture = true;

	U_LOG_W("Shared D3D11 texture imported: %ux%u, VkImage=%p, VkImageView=%p",
	        width, height, (void *)(uintptr_t)c->shared_image,
	        (void *)(uintptr_t)c->shared_view);
	return true;
}
#endif // XRT_OS_WINDOWS

static inline struct comp_vk_native_compositor *
vk_comp(struct xrt_compositor *xc)
{
	return (struct comp_vk_native_compositor *)xc;
}

/*
 *
 * xrt_compositor member functions
 *
 */

static xrt_result_t
vk_compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                               const struct xrt_swapchain_create_info *info,
                                               struct xrt_swapchain_create_properties *xsccp)
{
	xsccp->image_count = 3;
	xsccp->extra_bits = (enum xrt_swapchain_usage_bits)0;
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_create_swapchain(struct xrt_compositor *xc,
                                const struct xrt_swapchain_create_info *info,
                                struct xrt_swapchain **out_xsc)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	return comp_vk_native_swapchain_create(c, info, out_xsc);
}

static xrt_result_t
vk_compositor_import_swapchain(struct xrt_compositor *xc,
                                const struct xrt_swapchain_create_info *info,
                                struct xrt_image_native *native_images,
                                uint32_t image_count,
                                struct xrt_swapchain **out_xsc)
{
	return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
}

static xrt_result_t
vk_compositor_import_fence(struct xrt_compositor *xc,
                            xrt_graphics_sync_handle_t handle,
                            struct xrt_compositor_fence **out_xcf)
{
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
vk_compositor_create_semaphore(struct xrt_compositor *xc,
                                xrt_graphics_sync_handle_t *out_handle,
                                struct xrt_compositor_semaphore **out_xcsem)
{
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
vk_compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

	U_LOG_I("VK native compositor session begin - target=%p, renderer=%p",
	        (void *)c->target, (void *)c->renderer);

	// Notify the DP that the host activity is foregrounded. On Android
	// this propagates to the vendor SDK's on_resume hook so the plug-in
	// can re-enable face-tracking + restore backlight after a pause.
	xrt_display_processor_on_resume(c->display_processor);

	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_end_session(struct xrt_compositor *xc)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	U_LOG_I("VK native compositor session end");

	// Counterpart of begin_session: notify the DP we're backgrounding
	// so the vendor SDK can stop face-tracking + dim the backlight for
	// power. Safe even if the DP doesn't implement on_pause — the
	// helper NULL-checks the vtable slot.
	xrt_display_processor_on_pause(c->display_processor);

	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_predict_frame(struct xrt_compositor *xc,
                             int64_t *out_frame_id,
                             int64_t *out_wake_time_ns,
                             int64_t *out_predicted_gpu_time_ns,
                             int64_t *out_predicted_display_time_ns,
                             int64_t *out_predicted_display_period_ns)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

	c->frame_id++;
	*out_frame_id = c->frame_id;

	int64_t now_ns = (int64_t)os_monotonic_get_ns();
	int64_t period_ns = (int64_t)(U_TIME_1S_IN_NS / c->display_refresh_rate);

	// #867: measured wait_frame->scanout lookahead when available; the
	// period*2 constant only holds for an app keeping up at queue depth 1.
	int64_t lookahead_ns = period_ns * 2;
	if (c->target != NULL) {
		uint64_t measured = comp_vk_native_target_get_predicted_lookahead_ns(c->target);
		if (measured != 0) {
			lookahead_ns = (int64_t)measured;
		}
		comp_vk_native_target_mark_wait_frame(c->target);
	}
	*out_predicted_display_time_ns = now_ns + lookahead_ns;
	*out_predicted_display_period_ns = period_ns;
	*out_wake_time_ns = now_ns;
	*out_predicted_gpu_time_ns = period_ns;

	// The spec requires predictedDisplayTime to strictly increase across
	// xrWaitFrame calls, and CTS enforces it. The old period*2 constant
	// satisfied that for free — `now` only advances — but a MEASURED
	// lookahead can shrink between calls (the EMA moves), and if it shrinks
	// by more than `now` advanced the prediction would step backwards.
	// Clamp forward; `now` overtakes the floor again within a frame or two.
	if (*out_predicted_display_time_ns <= (int64_t)c->last_display_time_ns) {
		*out_predicted_display_time_ns = (int64_t)c->last_display_time_ns + 1;
	}
	c->last_display_time_ns = (uint64_t)*out_predicted_display_time_ns;

	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_wait_frame(struct xrt_compositor *xc,
                          int64_t *out_frame_id,
                          int64_t *out_predicted_display_time_ns,
                          int64_t *out_predicted_display_period_ns)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

#ifdef XRT_OS_WINDOWS
	if (c->owns_window && c->own_window != NULL &&
	    !comp_d3d11_window_is_valid(c->own_window)) {
		// #999: graceful exit request, not a lost session.
		U_LOG_I("Window closed - requesting session exit");
		return XRT_ERROR_COMPOSITOR_WINDOW_CLOSED;
	}
#endif

#ifdef XRT_OS_MACOS
	if (c->owns_window && c->macos_window != NULL &&
	    !comp_vk_native_window_macos_is_valid(c->macos_window)) {
		// #999: graceful exit request, not a lost session.
		U_LOG_I("Window closed - requesting session exit");
		return XRT_ERROR_COMPOSITOR_WINDOW_CLOSED;
	}
#endif

#ifdef XRT_OS_LINUX_DESKTOP
	if (c->owns_window && c->xcb_window != NULL &&
	    !comp_vk_native_window_xcb_is_valid(c->xcb_window)) {
		// #999: graceful exit request, not a lost session.
		U_LOG_I("Window closed - requesting session exit");
		return XRT_ERROR_COMPOSITOR_WINDOW_CLOSED;
	}
#endif

	int64_t period_ns = (int64_t)(U_TIME_1S_IN_NS / c->display_refresh_rate);

	c->frame_id++;
	*out_frame_id = c->frame_id;

	int64_t now_ns = (int64_t)os_monotonic_get_ns();
	// #867: measured wait_frame->scanout lookahead when available; the
	// period*2 constant only holds for an app keeping up at queue depth 1.
	int64_t lookahead_ns = period_ns * 2;
	if (c->target != NULL) {
		uint64_t measured = comp_vk_native_target_get_predicted_lookahead_ns(c->target);
		if (measured != 0) {
			lookahead_ns = (int64_t)measured;
		}
		comp_vk_native_target_mark_wait_frame(c->target);
	}
	*out_predicted_display_time_ns = now_ns + lookahead_ns;
	*out_predicted_display_period_ns = period_ns;

	// The spec requires predictedDisplayTime to strictly increase across
	// xrWaitFrame calls, and CTS enforces it. The old period*2 constant
	// satisfied that for free — `now` only advances — but a MEASURED
	// lookahead can shrink between calls (the EMA moves), and if it shrinks
	// by more than `now` advanced the prediction would step backwards.
	// Clamp forward; `now` overtakes the floor again within a frame or two.
	if (*out_predicted_display_time_ns <= (int64_t)c->last_display_time_ns) {
		*out_predicted_display_time_ns = (int64_t)c->last_display_time_ns + 1;
	}
	c->last_display_time_ns = (uint64_t)*out_predicted_display_time_ns;

	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_mark_frame(struct xrt_compositor *xc,
                          int64_t frame_id,
                          enum xrt_compositor_frame_point point,
                          int64_t when_ns)
{
	return XRT_SUCCESS;
}

/*!
 * #868: a target or atlas recreate invalidates everything the repaint replay
 * cached — the target images it would weave into, and the backdrop / mask /
 * atlas views it deliberately reuses rather than re-deriving. Disarm it and
 * drop the handles so no replay can resurrect them.
 *
 * Costs at most one frame of repaint: layer_commit re-arms unconditionally
 * (`c->repaint.armed = !zero_copy`) on the next real app frame, so this
 * self-heals without any re-arm bookkeeping here.
 *
 * Caller MUST hold c->mutex.
 */
static void
vk_repaint_disarm_locked(struct comp_vk_native_compositor *c)
{
	c->repaint.armed = false;
	c->repaint.backdrop_view = 0;
	c->repaint.backdrop_w = 0;
	c->repaint.backdrop_h = 0;
	c->repaint.mask_view = 0;
	c->repaint.atlas_image = 0;
	c->repaint.atlas_view = 0;
}

static xrt_result_t
vk_compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

#ifdef XRT_OS_WINDOWS
	if (c->hwnd != NULL) {
		RECT rect;
		if (GetClientRect((HWND)c->hwnd, &rect)) {
			uint32_t new_width = (uint32_t)(rect.right - rect.left);
			uint32_t new_height = (uint32_t)(rect.bottom - rect.top);

			if (new_width > 0 && new_height > 0 &&
			    (new_width != c->settings.preferred.width ||
			     new_height != c->settings.preferred.height)) {

				/*
				 * #868: the recreate below MUST hold the compositor lock.
				 *
				 * It destroys the target images, the swapchain and the DP's
				 * target-keyed caches, and the #868 repaint thread uses all
				 * three from its own thread under exactly this lock. Without
				 * it, a fullscreen transition tore the target down underneath
				 * a live repaint — the VK fullscreen crash. vkDeviceWaitIdle
				 * below drains the GPU and does nothing for that: the race is
				 * on the CPU, and a drain is not a mutex.
				 *
				 * d3d11_compositor_begin_frame and d3d12_compositor_begin_frame
				 * have always taken theirs here; VK was the outlier. Taken only
				 * on an actual size change, so the per-frame GetClientRect poll
				 * stays lock-free.
				 */
				os_mutex_lock(&c->mutex);

				// Before anything is torn down, so a repaint already queued
				// on the lock finds nothing to replay.
				vk_repaint_disarm_locked(c);

				U_LOG_I("Window resized: %ux%u -> %ux%u",
				        c->settings.preferred.width, c->settings.preferred.height,
				        new_width, new_height);

				if (c->target != NULL) {
					comp_vk_native_target_resize(c->target, new_width, new_height);
					// #602: drain the GPU after recreating the swapchain. The
					// steady-state per-frame path no longer calls
					// vkDeviceWaitIdle (it used to, incidentally, via the
					// renderer resize), so without this an in-flight frame can
					// present/sample a just-destroyed swapchain image →
					// VK_ERROR_DEVICE_LOST. Window resizes are rare, so the
					// stall here is fine.
					c->vk.vkDeviceWaitIdle(c->vk.device);
					// #602: the DP's target-handle-keyed caches are now
					// stale. The notification is NOT issued here — it is
					// issued once, for every recreate, at the acquire
					// boundary in vk_dp_weave_and_present (see
					// dp_notified_target_generation). Notifying only here
					// missed the recreate that target_acquire performs on
					// VK_ERROR_OUT_OF_DATE_KHR, which is how a stale cache
					// reached the GPU and lost the device. Deferring to the
					// acquire is safe: nothing uses the DP in between.
				}
				c->settings.preferred.width = new_width;
				c->settings.preferred.height = new_height;

				// #602: the atlas is owned by layer_commit for extension apps
				// (it allocates the stable mode atlas and derives the scaled
				// content view every frame). Only legacy apps — which skip the
				// layer_commit view resize — need the window-derived atlas
				// resize here. Driving it for extension apps fed a raw window
				// height the high-water then ratcheted on (→ device loss).
				if (c->legacy_app_tile_scaling) {
					uint32_t new_vw = new_width / 2;
					uint32_t new_vh = new_height;
					uint32_t tc, tr;
					comp_vk_native_renderer_get_tile_layout(c->renderer, &tc, &tr);
					comp_vk_native_renderer_resize(c->renderer, new_vw, new_vh,
					                                tc * new_vw, tr * new_vh);
				}

				os_mutex_unlock(&c->mutex);
			}
		}
	}
#endif

#ifdef XRT_OS_MACOS
	if (c->macos_window != NULL) {
		uint32_t new_width = 0, new_height = 0;
		comp_vk_native_window_macos_get_dimensions(c->macos_window, &new_width, &new_height);

		if (new_width > 0 && new_height > 0 &&
		    (new_width != c->settings.preferred.width ||
		     new_height != c->settings.preferred.height)) {

			// #868: same lock as the Windows branch — see the rationale there.
			os_mutex_lock(&c->mutex);
			vk_repaint_disarm_locked(c);

			U_LOG_I("Window resized: %ux%u -> %ux%u",
			        c->settings.preferred.width, c->settings.preferred.height,
			        new_width, new_height);

			// MoltenVK derives the surface currentExtent from the
			// CAMetalLayer drawableSize — sync it to the live view
			// bounds before recreating the swapchain (#524).
			comp_vk_native_window_macos_sync_drawable_size(c->macos_window);

			if (c->target != NULL) {
				comp_vk_native_target_resize(c->target, new_width, new_height);
				// #602: drain after swapchain recreate (see Windows branch).
				c->vk.vkDeviceWaitIdle(c->vk.device);
				// #602: DP cache invalidation happens at the acquire boundary
				// for every recreate path (see Windows branch).
			}
			c->settings.preferred.width = new_width;
			c->settings.preferred.height = new_height;

			// #602: extension apps' atlas is owned by layer_commit; only legacy
			// apps need the window-derived resize here (see Windows branch).
			if (c->legacy_app_tile_scaling) {
				uint32_t new_vw = new_width / 2;
				uint32_t new_vh = new_height;
				uint32_t tc, tr;
				comp_vk_native_renderer_get_tile_layout(c->renderer, &tc, &tr);
				comp_vk_native_renderer_resize(c->renderer, new_vw, new_vh,
				                                tc * new_vw, tr * new_vh);
			}

			os_mutex_unlock(&c->mutex);
		}
	}
#endif

#ifdef XRT_OS_LINUX_DESKTOP
	{
		uint32_t new_width = 0, new_height = 0;
		if (c->xcb_window != NULL) {
			comp_vk_native_window_xcb_get_dimensions(c->xcb_window, &new_width, &new_height);
		} else if (!c->owns_window && c->xcb_handle.connection != NULL) {
			// App-provided window (XR_DXR_xlib_window_binding): poll the live
			// geometry — no helper tracking ConfigureNotify for this window.
			comp_vk_native_window_xcb_query_geometry(&c->xcb_handle, &new_width, &new_height);
		}

		if (new_width > 0 && new_height > 0 &&
		    (new_width != c->settings.preferred.width ||
		     new_height != c->settings.preferred.height)) {

			// #868: same lock as the Windows branch — see the rationale there.
			os_mutex_lock(&c->mutex);
			vk_repaint_disarm_locked(c);

			U_LOG_I("Window resized: %ux%u -> %ux%u",
			        c->settings.preferred.width, c->settings.preferred.height,
			        new_width, new_height);

			if (c->target != NULL) {
				comp_vk_native_target_resize(c->target, new_width, new_height);
				// #602: drain after swapchain recreate (see Windows branch).
				c->vk.vkDeviceWaitIdle(c->vk.device);
				// #602: DP cache invalidation happens at the acquire boundary
				// for every recreate path (see Windows branch).
			}
			c->settings.preferred.width = new_width;
			c->settings.preferred.height = new_height;

			// #602: extension apps' atlas is owned by layer_commit; only legacy
			// apps need the window-derived resize here (see Windows branch).
			if (c->legacy_app_tile_scaling) {
				uint32_t new_vw = new_width / 2;
				uint32_t new_vh = new_height;
				uint32_t tc, tr;
				comp_vk_native_renderer_get_tile_layout(c->renderer, &tc, &tr);
				comp_vk_native_renderer_resize(c->renderer, new_vw, new_vh,
				                                tc * new_vw, tr * new_vh);
			}

			os_mutex_unlock(&c->mutex);
		}
	}
#endif

	c->layer_accum.layer_count = 0;
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	c->layer_accum.layer_count = 0;
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

	// #868: the app's submission window opens here and closes in layer_commit.
	// The compositor lock does NOT span it (layer_begin returns without
	// holding anything), so a repaint could otherwise win the lock partway
	// through the app filling layer_accum and replay a half-written frame.
	c->repaint.app_frame_in_progress = true;

	comp_layer_accum_begin(&c->layer_accum, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_projection(struct xrt_compositor *xc,
                                struct xrt_device *xdev,
                                struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_projection(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_projection_depth(struct xrt_compositor *xc,
                                      struct xrt_device *xdev,
                                      struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                      struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                      const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_projection_depth(&c->layer_accum, xsc, d_xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_quad(struct xrt_compositor *xc,
                          struct xrt_device *xdev,
                          struct xrt_swapchain *xsc,
                          const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_quad(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_cube(struct xrt_compositor *xc,
                          struct xrt_device *xdev,
                          struct xrt_swapchain *xsc,
                          const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_cube(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_cylinder(struct xrt_compositor *xc,
                              struct xrt_device *xdev,
                              struct xrt_swapchain *xsc,
                              const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_cylinder(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_equirect1(struct xrt_compositor *xc,
                               struct xrt_device *xdev,
                               struct xrt_swapchain *xsc,
                               const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_equirect1(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_equirect2(struct xrt_compositor *xc,
                               struct xrt_device *xdev,
                               struct xrt_swapchain *xsc,
                               const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_equirect2(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_passthrough(struct xrt_compositor *xc,
                                 struct xrt_device *xdev,
                                 const struct xrt_layer_data *data)
{
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_window_space(struct xrt_compositor *xc,
                                  struct xrt_device *xdev,
                                  struct xrt_swapchain *xsc,
                                  const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_window_space(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

/*!
 * Local-2D layer (XR_DXR_local_3d_zone v3, #439 Phase 3) — accumulate only;
 * the VK consumer is Windows leg 2 of
 * docs/roadmap/unified-2d-3d-phase3-impl.md §7 (un-parks the crossapi
 * §4 decision).
 */
static xrt_result_t
vk_compositor_layer_local_2d(struct xrt_compositor *xc,
                             struct xrt_device *xdev,
                             struct xrt_swapchain *xsc,
                             const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_local_2d(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

/*!
 * 3D display zone layer (XR_DXR_display_zones, ADR-027) — multi-swapchain
 * accumulate like projection; consumed by the zones-frame branch of
 * layer_commit (scaled blit into the window-spanning atlas at the zone rect).
 */
static xrt_result_t
vk_compositor_layer_zone_3d(struct xrt_compositor *xc,
                            struct xrt_device *xdev,
                            struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                            const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_zone_3d(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

/*!
 * Composite window-space (HUD) layers per-tile INTO the atlas image,
 * pre-weave, with proper alpha blending. Mirrors the per-tile rendering
 * model used by d3d11 / d3d12 / metal / gl, so atlas-capture parity
 * shows HUD on every tile and the DP weaver gives the layer per-eye
 * disparity (parallax) just like a projection layer.
 *
 * Atlas must be in SHADER_READ_ONLY_OPTIMAL on entry (the renderer
 * leaves it there after the projection-blit pass); on exit it is
 * returned to SHADER_READ_ONLY_OPTIMAL for the DP. No-op when no
 * window-space layers are present.
 *
 * @param c             The compositor.
 * @param cmd           Active command buffer to record into.
 * @param atlas_image   Atlas VkImage.
 * @param atlas_view    Atlas VkImageView (framebuffer attachment).
 * @param atlas_w       Allocated atlas width in pixels.
 * @param atlas_h       Allocated atlas height in pixels.
 * @param view_w        Per-tile view width.
 * @param view_h        Per-tile view height.
 * @param tile_columns  Atlas tile columns.
 * @param tile_rows     Atlas tile rows.
 */
static void
vk_compositor_render_window_space_into_atlas(struct comp_vk_native_compositor *c,
                                               VkCommandBuffer cmd,
                                               VkImage atlas_image,
                                               VkImageView atlas_view,
                                               uint32_t atlas_w,
                                               uint32_t atlas_h,
                                               uint32_t view_w,
                                               uint32_t view_h,
                                               uint32_t tile_columns,
                                               uint32_t tile_rows)
{
	struct vk_bundle *vk = &c->vk;

	bool has_ws = false;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		if (c->layer_accum.layers[i].data.type == XRT_LAYER_WINDOW_SPACE) {
			has_ws = true;
			break;
		}
	}
	if (!has_ws || tile_columns == 0 || tile_rows == 0 || view_w == 0 || view_h == 0) {
		return;
	}

	if (!c->window_space_blend.initialized && !c->window_space_blend_attempted) {
		c->window_space_blend_attempted = true;
		VkFormat atlas_fmt = (VkFormat)comp_vk_native_renderer_get_format(c->renderer);
		if (!vk_hud_blend_init(&c->window_space_blend, vk, atlas_fmt)) {
			U_LOG_E("[VK native] window-space alpha-blend init failed; "
			        "layers will be skipped");
		}
	}
	if (!c->window_space_blend.initialized) {
		return;
	}

	if (c->atlas_ws_fb == VK_NULL_HANDLE || c->atlas_ws_fb_view != atlas_view) {
		if (c->atlas_ws_fb != VK_NULL_HANDLE) {
			vk->vkDestroyFramebuffer(vk->device, c->atlas_ws_fb, NULL);
			c->atlas_ws_fb = VK_NULL_HANDLE;
		}
		VkFramebufferCreateInfo fb_ci = {
		    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		    .renderPass = c->window_space_blend.render_pass,
		    .attachmentCount = 1,
		    .pAttachments = &atlas_view,
		    .width = atlas_w,
		    .height = atlas_h,
		    .layers = 1,
		};
		if (vk->vkCreateFramebuffer(vk->device, &fb_ci, NULL, &c->atlas_ws_fb) != VK_SUCCESS) {
			U_LOG_E("[VK native] atlas framebuffer creation failed (%ux%u); "
			        "window-space layers will be skipped",
			        atlas_w, atlas_h);
			c->atlas_ws_fb = VK_NULL_HANDLE;
			return;
		}
		c->atlas_ws_fb_view = atlas_view;
	}

	// Atlas: SHADER_READ_ONLY_OPTIMAL → COLOR_ATTACHMENT_OPTIMAL.
	VkImageMemoryBarrier atlas_to_color = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .image = atlas_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    0, 0, NULL, 0, NULL, 1, &atlas_to_color);

	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];
		if (layer->data.type != XRT_LAYER_WINDOW_SPACE) {
			continue;
		}
		struct xrt_swapchain *xsc = layer->sc_array[0];
		if (xsc == NULL) {
			continue;
		}

		const struct xrt_layer_window_space_data *ws = &layer->data.window_space;
		uint32_t sc_index = ws->sub.image_index;

		VkImage src_image = (VkImage)(uintptr_t)
		    comp_vk_native_swapchain_get_image(xsc, sc_index);
		if (src_image == VK_NULL_HANDLE) {
			continue;
		}

		// Source: COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL.
		VkImageMemoryBarrier src_to_sample = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .image = src_image,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
		                         ws->sub.array_index, 1},
		};
		vk->vkCmdPipelineBarrier(cmd,
		    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		    0, 0, NULL, 0, NULL, 1, &src_to_sample);

		// Per-view pass with disparity shift in tile-fraction → atlas px.
		// Mirrors the metal/GL compositors (#413). The caller passes the
		// frame's EFFECTIVE content grid (#542): mono content arrives as a
		// 1×1 grid whose tile spans the full content region, so the old
		// hardware-keyed full-region special case is just the grid math.
		uint32_t effective_views = tile_columns * tile_rows;
		float half_disp = ws->disparity / 2.0f;
		for (uint32_t eye = 0; eye < effective_views; eye++) {
			uint32_t tile_x = eye % tile_columns;
			uint32_t tile_y = eye / tile_columns;

			float tile_origin_x = (float)(tile_x * view_w);
			float tile_origin_y = (float)(tile_y * view_h);
			float tile_w = (float)view_w;
			float tile_h = (float)view_h;

			// Per-view horizontal disparity, graded across the view sweep
			// (view index = baseline order, same as the projection views):
			// first view = -half, last = +half. Degenerates to the classic
			// -/+ pair for 2-view modes and to 0 for a single view.
			float eye_shift = 0.0f;
			if (effective_views > 1) {
				float t = (float)eye / (float)(effective_views - 1);
				eye_shift = -half_disp + ws->disparity * t;
			}

			int32_t dx = (int32_t)(tile_origin_x + (ws->x + eye_shift) * tile_w);
			int32_t dy = (int32_t)(tile_origin_y + ws->y * tile_h);
			int32_t dw_i = (int32_t)(ws->width * tile_w);
			int32_t dh_i = (int32_t)(ws->height * tile_h);
			if (dw_i <= 0 || dh_i <= 0) {
				continue;
			}

			// One-shot per-geometry diagnostic (#413): logs the resolved
			// per-view placement whenever the mode/layout/dims change, so a
			// missing-HUD report can be pinned to placement vs crop without
			// a custom build. Not per-frame (see debug-logging conventions).
			{
				static uint32_t logged_key = 0;
				uint32_t key = (tile_columns << 28) ^ (tile_rows << 24) ^
				               (effective_views << 20) ^ (view_w << 8) ^ view_h ^
				               ((uint32_t)c->hardware_display_3d << 31);
				if (key != logged_key && eye == 0) {
					logged_key = key;
					U_LOG_W("[VK native] window-space placement: 3d=%d tiles=%ux%u "
					        "views=%u view=%ux%u atlas=%ux%u first stamp dst=(%d,%d "
					        "%dx%d) ws=(%.3f,%.3f %.3fx%.3f disp=%.3f)",
					        (int)c->hardware_display_3d, tile_columns, tile_rows,
					        effective_views, view_w, view_h, atlas_w, atlas_h,
					        dx, dy, dw_i, dh_i,
					        ws->x, ws->y, ws->width, ws->height, ws->disparity);
				}
			}

			vk_hud_blend_draw_no_layout(&c->window_space_blend, vk, cmd,
			    c->atlas_ws_fb, atlas_w, atlas_h,
			    src_image, dx, dy, (uint32_t)dw_i, (uint32_t)dh_i);
		}

		// Source back to COLOR_ATTACHMENT_OPTIMAL so the app can rerender.
		VkImageMemoryBarrier src_back = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .image = src_image,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
		                         ws->sub.array_index, 1},
		};
		vk->vkCmdPipelineBarrier(cmd,
		    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		    0, 0, NULL, 0, NULL, 1, &src_back);
	}

	// Atlas: COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL for DP.
	VkImageMemoryBarrier atlas_back = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .image = atlas_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    0, 0, NULL, 0, NULL, 1, &atlas_back);
}

/*
 *
 * HUD overlay (shared u_hud system)
 *
 */

/*!
 * Render the diagnostic HUD overlay onto the target image.
 * Uses vkCmdBlitImage for an opaque blit (no alpha blending).
 * The u_hud pixel buffer has pre-composited semi-transparent background.
 */
static void
vk_compositor_render_hud(struct comp_vk_native_compositor *c,
                          VkCommandBuffer cmd,
                          VkImage target_image,
                          uint32_t target_width,
                          uint32_t target_height,
                          VkImageLayout target_layout)
{
	if (c->hud == NULL || !u_hud_is_visible()) {
		return;
	}

	struct vk_bundle *vk = &c->vk;

	// Frame timing
	uint64_t now_ns = os_monotonic_get_ns();
	float dt = (c->last_frame_ns > 0) ? (float)(now_ns - c->last_frame_ns) / 1e9f : 0.016f;
	c->last_frame_ns = now_ns;

	float dt_ms = dt * 1000.0f;
	if (dt_ms > 0.0f) {
		c->smoothed_frame_time_ms = c->smoothed_frame_time_ms * 0.9f + dt_ms * 0.1f;
	}
	float fps = (c->smoothed_frame_time_ms > 0.0f) ? (1000.0f / c->smoothed_frame_time_ms) : 0.0f;

	// Display dimensions and nominal viewer position from sys_info (fallback from DP)
	float disp_w_mm = 0, disp_h_mm = 0;
	float nom_x = 0, nom_y = 0, nom_z = 600.0f;
	if (c->sys_info_set) {
		disp_w_mm = c->sys_info.display_width_m * 1000.0f;
		disp_h_mm = c->sys_info.display_height_m * 1000.0f;
		nom_y = c->sys_info.nominal_viewer_y_m * 1000.0f;
		nom_z = c->sys_info.nominal_viewer_z_m * 1000.0f;
	} else if (c->display_processor != NULL) {
		float dw_m = 0, dh_m = 0;
		if (xrt_display_processor_get_display_dimensions(c->display_processor, &dw_m, &dh_m)) {
			disp_w_mm = dw_m * 1000.0f;
			disp_h_mm = dh_m * 1000.0f;
		}
	}

	// Eye positions from display processor
	struct xrt_eye_positions eye_pos = {0};
	if (c->display_processor != NULL) {
		xrt_display_processor_get_predicted_eye_positions(c->display_processor, &eye_pos);
	}
	if (!eye_pos.valid) {
		eye_pos.count = 2;
		eye_pos.eyes[0] = (struct xrt_eye_position){-0.032f, nom_y / 1000.0f, nom_z / 1000.0f};
		eye_pos.eyes[1] = (struct xrt_eye_position){ 0.032f, nom_y / 1000.0f, nom_z / 1000.0f};
	}

	// Fill HUD data
	struct u_hud_data data = {0};
	data.device_name = (c->xdev != NULL) ? c->xdev->str : "Unknown";
	data.fps = fps;
	data.frame_time_ms = c->smoothed_frame_time_ms;
	data.mode_3d = c->hardware_display_3d;
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			data.rendering_mode_name = c->xdev->rendering_modes[idx].mode_name;
		}
	}
	if (c->renderer != NULL) {
		uint32_t vw, vh;
		comp_vk_native_renderer_get_view_dimensions(c->renderer, &vw, &vh);
		data.render_width = vw;
		data.render_height = vh;
	}
	if (c->xdev != NULL && c->xdev->rendering_mode_count > 0) {
		u_tiling_compute_system_atlas(c->xdev->rendering_modes, c->xdev->rendering_mode_count,
		                              &data.swapchain_width, &data.swapchain_height);
	}
	data.window_width = target_width;
	data.window_height = target_height;
	data.display_width_mm = disp_w_mm;
	data.display_height_mm = disp_h_mm;
	data.nominal_x = nom_x;
	data.nominal_y = nom_y;
	data.nominal_z = nom_z;
	data.eye_count = eye_pos.count;
	for (uint32_t e = 0; e < eye_pos.count && e < 8; e++) {
		data.eyes[e].x = eye_pos.eyes[e].x * 1000.0f;
		data.eyes[e].y = eye_pos.eyes[e].y * 1000.0f;
		data.eyes[e].z = eye_pos.eyes[e].z * 1000.0f;
	}
	data.eye_tracking_active = eye_pos.is_tracking;

#ifdef XRT_BUILD_DRIVER_QWERTY
	if (c->xsysd != NULL) {
		struct xrt_pose qwerty_pose;
		if (qwerty_get_hmd_pose(c->xsysd->xdevs, c->xsysd->xdev_count, &qwerty_pose)) {
			data.vdisp_x = qwerty_pose.position.x;
			data.vdisp_y = qwerty_pose.position.y;
			data.vdisp_z = qwerty_pose.position.z;
			struct xrt_vec3 fwd_in = {0, 0, -1};
			struct xrt_vec3 fwd_out;
			math_quat_rotate_vec3(&qwerty_pose.orientation, &fwd_in, &fwd_out);
			data.forward_x = fwd_out.x;
			data.forward_y = fwd_out.y;
			data.forward_z = fwd_out.z;
		}

		struct qwerty_view_state ss;
		if (qwerty_get_view_state(c->xsysd->xdevs, c->xsysd->xdev_count, &ss)) {
			data.camera_mode = ss.camera_mode;
			data.ipd_factor = ss.ipd_factor;
			data.parallax_factor = ss.parallax_factor;
			data.inv_convergence_distance = ss.inv_convergence_distance;
			data.half_tan_vfov = ss.half_tan_vfov;
			data.m2v = ss.m2v;
			data.virtual_display_height = ss.virtual_display_height;
			data.perspective_factor = ss.perspective_factor;
			data.nominal_viewer_z = ss.nominal_viewer_z;
			data.screen_height_m = ss.screen_height_m;
		}
	}
#endif

	bool dirty = u_hud_update(c->hud, &data);

	uint32_t hud_w = u_hud_get_width(c->hud);
	uint32_t hud_h = u_hud_get_height(c->hud);

	// Lazy-create HUD VkImage (host-visible for CPU upload)
	if (c->hud_image == VK_NULL_HANDLE && hud_w > 0 && hud_h > 0) {
		VkImageCreateInfo image_ci = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType = VK_IMAGE_TYPE_2D,
		    .format = VK_FORMAT_R8G8B8A8_UNORM,
		    .extent = {hud_w, hud_h, 1},
		    .mipLevels = 1,
		    .arrayLayers = 1,
		    .samples = VK_SAMPLE_COUNT_1_BIT,
		    .tiling = VK_IMAGE_TILING_LINEAR,
		    .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		    .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
		};
		VkResult ret = vk->vkCreateImage(vk->device, &image_ci, NULL, &c->hud_image);
		if (ret != VK_SUCCESS) {
			U_LOG_E("Failed to create HUD VkImage: %d", ret);
			return;
		}

		VkMemoryRequirements mem_reqs;
		vk->vkGetImageMemoryRequirements(vk->device, c->hud_image, &mem_reqs);

		// Find host-visible memory type
		VkPhysicalDeviceMemoryProperties mem_props;
		vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);
		uint32_t mem_type = UINT32_MAX;
		for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
			if ((mem_reqs.memoryTypeBits & (1u << i)) &&
			    (mem_props.memoryTypes[i].propertyFlags &
			     (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
			        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
				mem_type = i;
				break;
			}
		}
		if (mem_type == UINT32_MAX) {
			U_LOG_E("No host-visible memory type for HUD image");
			vk->vkDestroyImage(vk->device, c->hud_image, NULL);
			c->hud_image = VK_NULL_HANDLE;
			return;
		}

		VkMemoryAllocateInfo alloc_info = {
		    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		    .allocationSize = mem_reqs.size,
		    .memoryTypeIndex = mem_type,
		};
		ret = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &c->hud_memory);
		if (ret != VK_SUCCESS) {
			U_LOG_E("Failed to allocate HUD memory: %d", ret);
			vk->vkDestroyImage(vk->device, c->hud_image, NULL);
			c->hud_image = VK_NULL_HANDLE;
			return;
		}
		vk->vkBindImageMemory(vk->device, c->hud_image, c->hud_memory, 0);

		c->hud_width = hud_w;
		c->hud_height = hud_h;
		dirty = true;
	}

	if (c->hud_image == VK_NULL_HANDLE) {
		return;
	}

	// Upload pixels if changed
	if (dirty) {
		const uint8_t *pixels = u_hud_get_pixels(c->hud);
		void *mapped = NULL;

		VkMemoryRequirements mem_reqs;
		vk->vkGetImageMemoryRequirements(vk->device, c->hud_image, &mem_reqs);

		VkResult ret = vk->vkMapMemory(vk->device, c->hud_memory, 0, mem_reqs.size, 0, &mapped);
		if (ret == VK_SUCCESS) {
			// Get subresource layout for proper row pitch
			VkImageSubresource subres = {
			    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			    .mipLevel = 0,
			    .arrayLayer = 0,
			};
			VkSubresourceLayout layout;
			vk->vkGetImageSubresourceLayout(vk->device, c->hud_image, &subres, &layout);

			uint32_t src_row_bytes = hud_w * 4;
			uint8_t *dst = (uint8_t *)mapped + layout.offset;
			for (uint32_t row = 0; row < hud_h; row++) {
				memcpy(dst + row * layout.rowPitch, pixels + row * src_row_bytes, src_row_bytes);
			}
			vk->vkUnmapMemory(vk->device, c->hud_memory);
		}
	}

	/*
	 * The target is NOT always in PRESENT_SRC_KHR here. A self-submitting DP
	 * runs its own render pass whose finalLayout leaves it in
	 * COLOR_ATTACHMENT_OPTIMAL, so the caller passes the layout it actually
	 * has. Hardcoding PRESENT_SRC_KHR declared a wrong oldLayout and tripped
	 * VUID-VkImageMemoryBarrier-oldLayout-01197 (20x per run measured on
	 * cube_zones_vk_win). Restored to the same layout on the way out so the
	 * caller's own follow-up barrier still finds what it expects.
	 */
	// Transition target from its current layout to TRANSFER_DST
	VkImageMemoryBarrier to_dst = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .oldLayout = target_layout,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .image = target_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    0, 0, NULL, 0, NULL, 1, &to_dst);

	// Transition HUD image to TRANSFER_SRC
	VkImageMemoryBarrier hud_to_src = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    .image = c->hud_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_HOST_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    0, 0, NULL, 0, NULL, 1, &hud_to_src);

	// Blit HUD to bottom-left corner of target
	uint32_t margin = 10;
	VkImageBlit blit = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .srcOffsets = {{0, 0, 0}, {(int32_t)hud_w, (int32_t)hud_h, 1}},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstOffsets = {
	        {(int32_t)margin, (int32_t)(target_height - hud_h - margin), 0},
	        {(int32_t)(margin + hud_w), (int32_t)(target_height - margin), 1},
	    },
	};
	vk->vkCmdBlitImage(cmd,
	    c->hud_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    1, &blit, VK_FILTER_NEAREST);

	// Transition HUD image back to PREINITIALIZED for next upload
	VkImageMemoryBarrier hud_back = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	    .dstAccessMask = VK_ACCESS_HOST_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
	    .image = c->hud_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_PIPELINE_STAGE_HOST_BIT,
	    0, 0, NULL, 0, NULL, 1, &hud_back);

	// Transition target back to whatever the caller handed us
	VkImageMemoryBarrier to_present = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .dstAccessMask = 0,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .newLayout = target_layout,
	    .image = target_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
	    0, 0, NULL, 0, NULL, 1, &to_present);
}

/*!
 * Lazily (re)create the DP input intermediate image at content dimensions.
 * Returns true on success.
 */
static bool
vk_ensure_dp_input_image(struct comp_vk_native_compositor *c,
                          uint32_t content_w, uint32_t content_h)
{
	struct vk_bundle *vk = &c->vk;

	if (c->dp_input_width == content_w && c->dp_input_height == content_h &&
	    c->dp_input_image != VK_NULL_HANDLE) {
		return true;
	}

	// Destroy old resources
	if (c->dp_input_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, c->dp_input_view, NULL);
		c->dp_input_view = VK_NULL_HANDLE;
	}
	if (c->dp_input_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, c->dp_input_image, NULL);
		c->dp_input_image = VK_NULL_HANDLE;
	}
	if (c->dp_input_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, c->dp_input_memory, NULL);
		c->dp_input_memory = VK_NULL_HANDLE;
	}

	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .extent = {content_w, content_h, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkResult res = vk->vkCreateImage(vk->device, &image_ci, NULL, &c->dp_input_image);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create VK DP input image %ux%u: %d", content_w, content_h, res);
		return false;
	}

	VkMemoryRequirements mem_reqs;
	vk->vkGetImageMemoryRequirements(vk->device, c->dp_input_image, &mem_reqs);

	uint32_t mem_type_index = 0;
	VkPhysicalDeviceMemoryProperties mem_props;
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((mem_reqs.memoryTypeBits & (1 << i)) &&
		    (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			mem_type_index = i;
			break;
		}
	}

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = mem_reqs.size,
	    .memoryTypeIndex = mem_type_index,
	};

	res = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &c->dp_input_memory);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to allocate VK DP input memory: %d", res);
		return false;
	}

	res = vk->vkBindImageMemory(vk->device, c->dp_input_image, c->dp_input_memory, 0);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to bind VK DP input memory: %d", res);
		return false;
	}

	VkImageViewCreateInfo view_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = c->dp_input_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .subresourceRange = {
	        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	        .baseMipLevel = 0,
	        .levelCount = 1,
	        .baseArrayLayer = 0,
	        .layerCount = 1,
	    },
	};

	res = vk->vkCreateImageView(vk->device, &view_ci, NULL, &c->dp_input_view);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create VK DP input view: %d", res);
		return false;
	}

	c->dp_input_width = content_w;
	c->dp_input_height = content_h;
	U_LOG_I("VK crop: created DP input image %ux%u", content_w, content_h);

	return true;
}

/*!
 * Record crop-blit commands to copy the content region from an oversized atlas
 * into the DP input intermediate image. Updates *src_image_u64 and *src_view_u64
 * to point to the intermediate if cropping was performed.
 */
static void
vk_crop_atlas_for_dp(struct comp_vk_native_compositor *c,
                      VkCommandBuffer cmd,
                      uint64_t *src_image_u64,
                      uint64_t *src_view_u64,
                      uint32_t content_w,
                      uint32_t content_h,
                      uint32_t atlas_w,
                      uint32_t atlas_h)
{
	if (content_w == atlas_w && content_h == atlas_h) {
		return;
	}

	if (!vk_ensure_dp_input_image(c, content_w, content_h)) {
		return; // fallback: pass oversized atlas
	}

	struct vk_bundle *vk = &c->vk;
	VkImage src_image = (VkImage)(uintptr_t)*src_image_u64;

	// Transition src → TRANSFER_SRC
	VkImageMemoryBarrier src_to_transfer = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    .image = src_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    0, 0, NULL, 0, NULL, 1, &src_to_transfer);

	// Transition intermediate → TRANSFER_DST
	VkImageMemoryBarrier dst_to_transfer = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = 0,
	    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .image = c->dp_input_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    0, 0, NULL, 0, NULL, 1, &dst_to_transfer);

	// Copy content region
	VkImageCopy region = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .srcOffset = {0, 0, 0},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstOffset = {0, 0, 0},
	    .extent = {content_w, content_h, 1},
	};
	vk->vkCmdCopyImage(cmd,
	    src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    c->dp_input_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    1, &region);

	// Transition src back → SHADER_READ_ONLY
	VkImageMemoryBarrier src_back = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .image = src_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};

	// Transition intermediate → SHADER_READ_ONLY (for DP sampling)
	VkImageMemoryBarrier dst_to_read = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .image = c->dp_input_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};

	VkImageMemoryBarrier barriers[2] = {src_back, dst_to_read};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    0, 0, NULL, 0, NULL, 2, barriers);

	// Update output pointers to intermediate
	*src_image_u64 = (uint64_t)(uintptr_t)c->dp_input_image;
	*src_view_u64 = (uint64_t)(uintptr_t)c->dp_input_view;
}

/*
 *
 * MCP capture helpers
 *
 */

// Helper: find a host-visible memory type.
static bool
vk_native_find_host_visible_memory_type(struct vk_bundle *vk, uint32_t type_bits, uint32_t *out_index)
{
	VkPhysicalDeviceMemoryProperties props;
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &props);
	const VkMemoryPropertyFlags want =
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) {
			*out_index = i;
			return true;
		}
	}
	return false;
}

// Read the content region of the renderer's atlas (tile_columns × view_width
// by tile_rows × view_height — what actually got composited, matching what
// the compositor crops and sends to the DP) into a host-visible staging
// buffer, swap channels if format is BGRA, and write @p path as PNG. Uses
// the renderer's command pool and the main queue.
static bool
vk_native_capture_atlas_to_png(struct comp_vk_native_compositor *c, const char *path)
{
	struct vk_bundle *vk = &c->vk;

	// The DP's actual input this frame — the renderer atlas, or the zero-copy
	// app swapchain — recorded in layer_commit. Fall back to the renderer atlas
	// if a capture somehow fires before layer_commit set it.
	uint64_t atlas_image_u64 = c->capture_src_image_u64 != 0
	    ? c->capture_src_image_u64
	    : comp_vk_native_renderer_get_atlas_image(c->renderer);
	VkImage atlas_image = (VkImage)(uintptr_t)atlas_image_u64;
	if (atlas_image == VK_NULL_HANDLE) {
		return false;
	}

	// #542: capture the frame's effective content region (what the renderer
	// painted), falling back to the mode layout pre-first-commit.
	uint32_t tile_columns = 1, tile_rows = 1;
	uint32_t view_w = 0, view_h = 0;
	if (c->eff_layout.views > 0 && c->eff_layout.tile_w > 0 && c->eff_layout.tile_h > 0) {
		tile_columns = c->eff_layout.cols;
		tile_rows = c->eff_layout.rows;
		view_w = c->eff_layout.tile_w;
		view_h = c->eff_layout.tile_h;
	} else {
		comp_vk_native_renderer_get_tile_layout(c->renderer, &tile_columns, &tile_rows);
		comp_vk_native_renderer_get_view_dimensions(c->renderer, &view_w, &view_h);
	}
	if (tile_columns == 0 || tile_rows == 0 || view_w == 0 || view_h == 0) {
		return false;
	}
	uint32_t atlas_w = c->capture_src_atlas_w, atlas_h = c->capture_src_atlas_h;
	if (atlas_w == 0 || atlas_h == 0) {
		comp_vk_native_renderer_get_atlas_dimensions(c->renderer, &atlas_w, &atlas_h);
	}

	uint32_t content_w = tile_columns * view_w;
	uint32_t content_h = tile_rows * view_h;
	if (atlas_w > 0 && content_w > atlas_w) content_w = atlas_w;
	if (atlas_h > 0 && content_h > atlas_h) content_h = atlas_h;

	VkFormat atlas_format = (VkFormat)(c->capture_src_format != 0
	    ? c->capture_src_format
	    : comp_vk_native_renderer_get_format(c->renderer));
	bool swap_bgra =
	    (atlas_format == VK_FORMAT_B8G8R8A8_UNORM || atlas_format == VK_FORMAT_B8G8R8A8_SRGB);

	VkCommandPool cmd_pool = (VkCommandPool)(uintptr_t)comp_vk_native_renderer_get_cmd_pool(c->renderer);
	if (cmd_pool == VK_NULL_HANDLE) {
		return false;
	}

	// Allocate host-visible staging buffer (content_w × content_h × 4).
	VkDeviceSize bytes = (VkDeviceSize)content_w * (VkDeviceSize)content_h * 4;
	VkBuffer staging_buf = VK_NULL_HANDLE;
	VkDeviceMemory staging_mem = VK_NULL_HANDLE;
	uint8_t *mapped = NULL;

	VkBufferCreateInfo bi = {
	    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .size = bytes,
	    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (vk->vkCreateBuffer(vk->device, &bi, NULL, &staging_buf) != VK_SUCCESS) {
		return false;
	}

	VkMemoryRequirements mreq;
	vk->vkGetBufferMemoryRequirements(vk->device, staging_buf, &mreq);
	uint32_t mem_type = 0;
	if (!vk_native_find_host_visible_memory_type(vk, mreq.memoryTypeBits, &mem_type)) {
		vk->vkDestroyBuffer(vk->device, staging_buf, NULL);
		return false;
	}
	VkMemoryAllocateInfo ai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = mreq.size,
	    .memoryTypeIndex = mem_type,
	};
	if (vk->vkAllocateMemory(vk->device, &ai, NULL, &staging_mem) != VK_SUCCESS ||
	    vk->vkBindBufferMemory(vk->device, staging_buf, staging_mem, 0) != VK_SUCCESS) {
		if (staging_mem != VK_NULL_HANDLE) vk->vkFreeMemory(vk->device, staging_mem, NULL);
		vk->vkDestroyBuffer(vk->device, staging_buf, NULL);
		return false;
	}

	// One-shot command buffer to copy atlas -> buffer.
	VkCommandBufferAllocateInfo cbai = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = cmd_pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vk->vkAllocateCommandBuffers(vk->device, &cbai, &cmd) != VK_SUCCESS) {
		vk->vkFreeMemory(vk->device, staging_mem, NULL);
		vk->vkDestroyBuffer(vk->device, staging_buf, NULL);
		return false;
	}

	VkCommandBufferBeginInfo cbi = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->vkBeginCommandBuffer(cmd, &cbi);

	// Atlas was sampled by the DP/weaver this frame, so its layout is
	// SHADER_READ_ONLY_OPTIMAL by the time we're called from layer_commit.
	// Transition: SHADER_READ_ONLY_OPTIMAL → TRANSFER_SRC_OPTIMAL → SHADER_READ_ONLY_OPTIMAL.
	VkImageMemoryBarrier b = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = atlas_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	};
	vk->vkCmdPipelineBarrier(cmd,
	                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                         VK_PIPELINE_STAGE_TRANSFER_BIT,
	                         0, 0, NULL, 0, NULL, 1, &b);

	VkBufferImageCopy region = {
	    .bufferOffset = 0,
	    .bufferRowLength = 0,
	    .bufferImageHeight = 0,
	    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .imageOffset = {0, 0, 0},
	    .imageExtent = {content_w, content_h, 1},
	};
	vk->vkCmdCopyImageToBuffer(cmd, atlas_image,
	                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                           staging_buf, 1, &region);

	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vk->vkCmdPipelineBarrier(cmd,
	                         VK_PIPELINE_STAGE_TRANSFER_BIT,
	                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                         0, 0, NULL, 0, NULL, 1, &b);

	vk->vkEndCommandBuffer(cmd);

	VkSubmitInfo si = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	};
	vk->vkQueueSubmit(vk->main_queue->queue, 1, &si, VK_NULL_HANDLE);
	vk->vkQueueWaitIdle(vk->main_queue->queue);
	vk->vkFreeCommandBuffers(vk->device, cmd_pool, 1, &cmd);

	// Read pixels and encode PNG.
	bool ok = false;
	if (vk->vkMapMemory(vk->device, staging_mem, 0, bytes, 0, (void **)&mapped) == VK_SUCCESS) {
		uint8_t *pixels = mapped;
		uint8_t *swapped = NULL;
		if (swap_bgra) {
			swapped = malloc((size_t)bytes);
			if (swapped != NULL) {
				for (VkDeviceSize i = 0; i < bytes; i += 4) {
					swapped[i + 0] = mapped[i + 2];
					swapped[i + 1] = mapped[i + 1];
					swapped[i + 2] = mapped[i + 0];
					swapped[i + 3] = mapped[i + 3];
				}
				pixels = swapped;
			}
		}
		// Force opaque: swapchain alpha is undefined for display output, and
		// left as-is the PNG renders transparent/black (issue #425). Covers
		// both the BGRA-swapped copy and the direct (mapped) path.
		u_image_force_opaque_rgba8(pixels, content_w, content_h, (size_t)content_w * 4);
		ok = stbi_write_png(path, (int)content_w, (int)content_h, 4,
		                    pixels, (int)content_w * 4) != 0;
		free(swapped);
		vk->vkUnmapMemory(vk->device, staging_mem);
	}

	vk->vkFreeMemory(vk->device, staging_mem, NULL);
	vk->vkDestroyBuffer(vk->device, staging_buf, NULL);
	return ok;
}

// Composite-tap diagnostics (#833 debugging): dump one BGRA8 image to PNG.
// Same staging pattern as vk_native_capture_atlas_to_png but parameterized on
// image/dims/layout, and optionally KEEPING alpha (the flattened 2D scratch's
// alpha is the diagnostic signal). Restores the image to @p layout.
static bool
vk_native_dump_image_to_png(struct comp_vk_native_compositor *c,
                            VkImage image,
                            uint32_t w,
                            uint32_t h,
                            VkImageLayout layout,
                            VkAccessFlags access,
                            VkPipelineStageFlags stage,
                            bool keep_alpha,
                            const char *path)
{
	struct vk_bundle *vk = &c->vk;
	if (image == VK_NULL_HANDLE || w == 0 || h == 0) {
		return false;
	}
	VkCommandPool cmd_pool = (VkCommandPool)(uintptr_t)comp_vk_native_renderer_get_cmd_pool(c->renderer);
	if (cmd_pool == VK_NULL_HANDLE) {
		return false;
	}

	VkDeviceSize bytes = (VkDeviceSize)w * (VkDeviceSize)h * 4;
	VkBuffer staging_buf = VK_NULL_HANDLE;
	VkDeviceMemory staging_mem = VK_NULL_HANDLE;
	uint8_t *mapped = NULL;

	VkBufferCreateInfo bi = {
	    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .size = bytes,
	    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (vk->vkCreateBuffer(vk->device, &bi, NULL, &staging_buf) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements mreq;
	vk->vkGetBufferMemoryRequirements(vk->device, staging_buf, &mreq);
	uint32_t mem_type = 0;
	if (!vk_native_find_host_visible_memory_type(vk, mreq.memoryTypeBits, &mem_type)) {
		vk->vkDestroyBuffer(vk->device, staging_buf, NULL);
		return false;
	}
	VkMemoryAllocateInfo ai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = mreq.size,
	    .memoryTypeIndex = mem_type,
	};
	if (vk->vkAllocateMemory(vk->device, &ai, NULL, &staging_mem) != VK_SUCCESS ||
	    vk->vkBindBufferMemory(vk->device, staging_buf, staging_mem, 0) != VK_SUCCESS) {
		if (staging_mem != VK_NULL_HANDLE) vk->vkFreeMemory(vk->device, staging_mem, NULL);
		vk->vkDestroyBuffer(vk->device, staging_buf, NULL);
		return false;
	}

	VkCommandBufferAllocateInfo cbai = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = cmd_pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vk->vkAllocateCommandBuffers(vk->device, &cbai, &cmd) != VK_SUCCESS) {
		vk->vkFreeMemory(vk->device, staging_mem, NULL);
		vk->vkDestroyBuffer(vk->device, staging_buf, NULL);
		return false;
	}
	VkCommandBufferBeginInfo cbi = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->vkBeginCommandBuffer(cmd, &cbi);

	VkImageMemoryBarrier b = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .oldLayout = layout,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    .srcAccessMask = access,
	    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	};
	vk->vkCmdPipelineBarrier(cmd, stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

	VkBufferImageCopy region = {
	    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .imageExtent = {w, h, 1},
	};
	vk->vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_buf, 1, &region);

	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b.newLayout = layout;
	b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b.dstAccessMask = access;
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, stage, 0, 0, NULL, 0, NULL, 1, &b);

	vk->vkEndCommandBuffer(cmd);
	VkSubmitInfo si = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	};
	vk->vkQueueSubmit(vk->main_queue->queue, 1, &si, VK_NULL_HANDLE);
	vk->vkQueueWaitIdle(vk->main_queue->queue);
	vk->vkFreeCommandBuffers(vk->device, cmd_pool, 1, &cmd);

	bool ok = false;
	if (vk->vkMapMemory(vk->device, staging_mem, 0, bytes, 0, (void **)&mapped) == VK_SUCCESS) {
		// Composite target + scratches are B8G8R8A8_UNORM — swap to RGBA.
		uint8_t *rgba = malloc((size_t)bytes);
		if (rgba != NULL) {
			for (VkDeviceSize i = 0; i < bytes; i += 4) {
				rgba[i + 0] = mapped[i + 2];
				rgba[i + 1] = mapped[i + 1];
				rgba[i + 2] = mapped[i + 0];
				rgba[i + 3] = mapped[i + 3];
			}
			if (!keep_alpha) {
				u_image_force_opaque_rgba8(rgba, w, h, (size_t)w * 4);
			}
			ok = stbi_write_png(path, (int)w, (int)h, 4, rgba, (int)w * 4) != 0;
			free(rgba);
		}
		vk->vkUnmapMemory(vk->device, staging_mem);
	}
	vk->vkFreeMemory(vk->device, staging_mem, NULL);
	vk->vkDestroyBuffer(vk->device, staging_buf, NULL);
	return ok;
}

// Composite-tap trigger (#833 debugging): when %TEMP%\displayxr_composite_tap_trigger
// exists, dump this frame's final target / weave snapshot / flattened 2D
// scratch next to it and delete the trigger. Windows-only diagnostic.
static void
vk_native_dispatch_composite_tap(struct comp_vk_native_compositor *c)
{
#ifdef XRT_OS_WINDOWS
	if (c->tap_target_image == VK_NULL_HANDLE) {
		return;
	}
	static char trigger[512] = {0};
	static char out_dir[512] = {0};
	if (trigger[0] == '\0') {
		const char *tmp = getenv("TEMP");
		if (tmp == NULL || tmp[0] == '\0') {
			tmp = "C:\\Temp";
		}
		snprintf(trigger, sizeof(trigger), "%s\\displayxr_composite_tap_trigger", tmp);
		snprintf(out_dir, sizeof(out_dir), "%s", tmp);
	}
	if (GetFileAttributesA(trigger) == INVALID_FILE_ATTRIBUTES) {
		return;
	}
	DeleteFileA(trigger);

	char path[600];
	snprintf(path, sizeof(path), "%s\\displayxr_tap_target.png", out_dir);
	bool ok_t = vk_native_dump_image_to_png(c, c->tap_target_image, c->tap_target_w, c->tap_target_h,
	                                        c->tap_target_layout,
	                                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, false, path);
	snprintf(path, sizeof(path), "%s\\displayxr_tap_weave.png", out_dir);
	bool ok_w = vk_native_dump_image_to_png(c, c->weave_scratch, c->tap_region_w, c->tap_region_h,
	                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                                        VK_ACCESS_SHADER_READ_BIT,
	                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, false, path);
	snprintf(path, sizeof(path), "%s\\displayxr_tap_twod.png", out_dir);
	bool ok_2 = vk_native_dump_image_to_png(c, c->local2d_scratch, c->tap_region_w, c->tap_region_h,
	                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                                        VK_ACCESS_SHADER_READ_BIT,
	                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, true, path);
	U_LOG_W("VK composite tap: target=%d weave=%d twod=%d (%ux%u region %ux%u) -> %s",
	        ok_t, ok_w, ok_2, c->tap_target_w, c->tap_target_h, c->tap_region_w, c->tap_region_h,
	        out_dir);
#else
	(void)c;
#endif
}

// Run the capture readback if the per-frame intent matches @p mode_filter.
// Caller polls intent at top of layer_commit; this fires at each boundary.
static void
vk_native_dispatch_capture(struct comp_vk_native_compositor *c, uint32_t mode_filter)
{
	if (!u_capture_intent_should_capture(&c->capture_intent, mode_filter)) {
		return;
	}
	bool ok = vk_native_capture_atlas_to_png(c, c->capture_intent.path);
	if (ok) {
		U_LOG_I("Atlas captured (mode=%u) to %s",
		        c->capture_intent.mode, c->capture_intent.path);
	} else {
		U_LOG_W("Atlas capture failed (mode=%u path=%s)",
		        c->capture_intent.mode, c->capture_intent.path);
	}
	u_capture_intent_complete(&c->capture_intent, &c->mcp_capture, ok);
}


// #439 Phase 3 — masked 2D-over-3D composite, defined below near the zone-mask
// API. Called from the weave path in layer_commit; release from destroy.
static bool
vk_composite_local_2d(struct comp_vk_native_compositor *c,
                      VkCommandBuffer cmd,
                      VkImage dst_image,
                      VkImageView dst_view,
                      uint32_t dst_w,
                      uint32_t dst_h,
                      VkImageLayout dst_incoming,
                      VkImageLayout dst_outgoing,
                      bool reuse_twod);
// #491 part 3 — pre-weave 2D-under backdrop flatten (defined below near the
// composite). Called before process_atlas; result handed to the DP.
static VkImageView
vk_flatten_backdrop_2d(struct comp_vk_native_compositor *c,
                       VkCommandBuffer cmd,
                       uint32_t dst_w,
                       uint32_t dst_h,
                       uint32_t *out_w,
                       uint32_t *out_h);
static void
vk_release_local2d_state(struct comp_vk_native_compositor *c);
// runtime#757 / LeiaSR#85 — push the app window's panel-relative origin to the DP
// (windowed-weaving phase anchor). Defined below, next to get_window_metrics.
static void
vk_update_present_origin(struct comp_vk_native_compositor *c);
// #224 / ADR-027 hardware-DP zone leg (P4): one-time caps probe + per-frame
// sideband publish of the wish / sticky mask. Defined with the other zone
// helpers near the bottom.
static bool
vk_zone_dp_supported(struct comp_vk_native_compositor *c);
static void
vk_sync_zone_mask_to_dp(struct comp_vk_native_compositor *c);

// Per-frame effective CONTENT layout (#542) — same policy as the D3D11/D3D12/
// GL legs: the content recipe is the ACTIVE MODE's, submissions are clamped
// to it (always-stereo apps submit identical views in a mono mode; zone
// layers carry zone-sized imageRects). The hardware weave-state never clamps
// content — divergence is the hardware-state override
// (xrRequestDisplayModeDXR), under which this layout keeps following the
// mode and the DP keeps weaving.
static void
vk_compute_effective_layout(struct comp_vk_native_compositor *c)
{
	uint32_t mode_cols = 1, mode_rows = 1;
	uint32_t view_w = 0, view_h = 0;
	comp_vk_native_renderer_get_tile_layout(c->renderer, &mode_cols, &mode_rows);
	comp_vk_native_renderer_get_view_dimensions(c->renderer, &view_w, &view_h);
	if (mode_cols == 0) mode_cols = 1;
	if (mode_rows == 0) mode_rows = 1;
	uint32_t mode_tiles = mode_cols * mode_rows;

	uint32_t views = mode_tiles;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		if (c->layer_accum.layers[i].data.type == XRT_LAYER_PROJECTION ||
		    c->layer_accum.layers[i].data.type == XRT_LAYER_PROJECTION_DEPTH ||
		    c->layer_accum.layers[i].data.type == XRT_LAYER_ZONE_3D) {
			views = c->layer_accum.layers[i].data.view_count;
			break;
		}
	}
	if (views == 0) {
		views = 1;
	}
	if (views > mode_tiles) {
		views = mode_tiles;
	}
	if (views > XRT_MAX_VIEWS) {
		views = XRT_MAX_VIEWS;
	}

	c->eff_layout.views = views;
	if (views == 1) {
		c->eff_layout.cols = 1;
		c->eff_layout.rows = 1;
		c->eff_layout.tile_w = mode_cols * view_w;
		c->eff_layout.tile_h = mode_rows * view_h;
	} else {
		c->eff_layout.cols = mode_cols;
		c->eff_layout.rows = mode_rows;
		c->eff_layout.tile_w = view_w;
		c->eff_layout.tile_h = view_h;
	}
}

/*!
 * #837 frame-stage accumulator. File-scope rather than a function-local
 * static because both the app frame and the repaint replay record into it
 * through the shared weave function below.
 */
static struct vk_frame_timing s_ftiming = {0};

/*!
 * The canvas sub-rect this compositor hands `xrt_display_processor_process_atlas`,
 * window-relative — the ONE place that answers "what region will the DP write".
 *
 * In-process the answer is always the degenerate rect, i.e. "fill the whole
 * target". A zones frame is not an exception: each zone rect is its own canvas
 * and drives the lens mask, while the weave output rect stays the full client
 * window (same rule `vk_effective_canvas` encodes for the view dims). The
 * out-of-process `comp_multi` path differs — it passes the frame's zone-3D rect
 * down and the DP confines the weave to it.
 *
 * It exists as a function, and is read by the `process_atlas` call sites rather
 * than open-coded there, because the compose-under backdrop must be cut to
 * exactly this region: when the two disagree the backdrop is stretched by
 * `target_h / canvas_h` (#1101). One authority, no per-call-site math.
 */
static struct xrt_rect
vk_dp_canvas_rect(struct comp_vk_native_compositor *c)
{
	(void)c;
	return (struct xrt_rect){0};
}


/*
 * Compose-under backdrop for the base-DP slot-16 seam (#1073), in-process leg.
 *
 * ## Why this exists twice
 *
 * The weave assigns views per *subpixel* while RGBA carries one alpha per
 * *pixel*, so a mixed-alpha pixel — the silhouette, the parallax de-occlusion
 * band — has no correct alpha at all. The fix is to composite an opaque
 * background under every view BEFORE the per-subpixel collapse, which is what
 * slot 16 delivers. `comp_multi_system.c` does this for the out-of-process
 * session path; this is the same producer for the in-process one, and it is
 * not a duplicate — `comp_bg2d` is shared verbatim, only the geometry the
 * compositor happens to know differs.
 *
 * It matters because the two paths differ in exactly the way that decides
 * whether the result is shippable. Out of process the weave lands on the
 * service's `TYPE_APPLICATION_OVERLAY`, which carries the ≤ 0.80
 * anti-tapjacking alpha clamp — a 20 % launcher ghost over every pixel that no
 * backdrop can remove. In process (ADR-036 Architecture A) the app owns a plain
 * translucent `TYPE_APPLICATION` window: no clamp, no overlay at all, so an
 * opaque-band weave over real captured pixels is the end state rather than a
 * demonstration of one.
 *
 * ## Precedence: an app-supplied Local2D backdrop wins
 *
 * `vk_flatten_backdrop_2d` (#491) already owns this slot when the frame has
 * 2D-UNDER Local2D layers: the app explicitly said "this is what is behind my
 * 3D content", and a captured screenshot is a *guess* at the same question. An
 * explicit answer beats a guess, so the capture only fills the slot on frames
 * where the flatten declined it. The two are never blended — one background,
 * one authority.
 *
 * Returns VK_NULL_HANDLE (leaving @p w / @p h at 0) whenever there is nothing
 * to supply, which is byte-for-byte the pre-#1073 path.
 */
static VkImageView
vk_bg2d_backdrop(struct comp_vk_native_compositor *c, uint32_t *w, uint32_t *h)
{
	*w = 0;
	*h = 0;
#ifdef XRT_OS_ANDROID
	// Same two gates as the OOP path: a backdrop is meaningless under an
	// opaque present, and the whole producer is off unless asked for.
	if (!c->transparent_background || !comp_bg2d_enabled() || c->display_processor == NULL) {
		return VK_NULL_HANDLE;
	}

	// Where the backdrop must be cut from, in panel pixels. A T2 producer sends
	// whole-PANEL pixels while slot 16 promises the DP a backdrop already in the
	// region it will map onto, so comp_bg2d crops to this rect (#174) — and
	// re-crops whenever it moves, which is what makes a device rotation
	// self-correcting.
	//
	// In-process the window rect comes from the app's own
	// xrSetAndroidWindowGeometryDXR publish (#1037) through the same
	// android_globals sink the OOP path reads (#1033). The region *inside* it is
	// whatever `process_atlas` is handed as its canvas sub-rect — read from the
	// one authority below, never re-derived here, because deriving it from the
	// frame's zone-3D rect while the DP is told to fill the whole target is
	// exactly the #1101 vertical stretch.
	int32_t win_x = 0, win_y = 0, display_id = -1;
	uint32_t win_w = 0, win_h = 0, panel_w = 0, panel_h = 0;
	uint64_t generation = 0;
	if (!android_globals_get_window_screen_rect(&win_x, &win_y, &win_w, &win_h, &display_id, &panel_w, &panel_h,
	                                            &generation)) {
		// No geometry published: cropping would be a guess, and a whole-panel
		// backdrop stretched across the canvas is exactly the mis-registration
		// #174 was about. Sit it out.
		return VK_NULL_HANDLE;
	}
	if (panel_w == 0 || panel_h == 0 || win_w == 0 || win_h == 0) {
		return VK_NULL_HANDLE;
	}

	const struct xrt_rect window_on_panel = {
	    .offset = {.w = win_x, .h = win_y},
	    .extent = {.w = (int)win_w, .h = (int)win_h},
	};
	const struct xrt_rect dp_canvas = vk_dp_canvas_rect(c);

	struct xrt_rect canvas_on_panel = {0};
	if (!comp_bg2d_backdrop_source_rect(&window_on_panel, &dp_canvas, &canvas_on_panel)) {
		return VK_NULL_HANDLE;
	}

	struct vk_bundle *vk = &c->vk;
	VkCommandPool pool = (VkCommandPool)(uintptr_t)comp_vk_native_renderer_get_cmd_pool(c->renderer);
	return comp_bg2d_ensure(&c->bg2d, vk, pool, &canvas_on_panel, panel_w, panel_h, w, h);
#else
	(void)c;
	return VK_NULL_HANDLE;
#endif
}

/*!
 * #868: acquire -> weave -> submit -> present. Shared by the app frame and by
 * the repaint replay, so a repaint is constructed exactly like the frame it
 * stands in for rather than by a second code path that has to be kept in
 * agreement with it (which is the bug class that cost D3D11 three fixes).
 *
 * Caller MUST hold c->mutex: this submits to the app's VkQueue and drives the
 * vendor weaver, and neither tolerates two concurrent callers.
 *
 * On a repaint, three things are deliberately NOT re-done, because each reads
 * state the app owns and has since overwritten:
 *   - the window-space layer composite into the atlas,
 *   - the 2D-under backdrop flatten (cached in c->repaint.backdrop_view),
 *   - the app-frame timing marks (see comp_vk_native_target_weave_mark_repaint).
 * The crop IS re-done: it reads the compositor-owned atlas and writes
 * compositor-owned scratch, and skipping it hands the DP a stale image.
 */
static xrt_result_t
vk_dp_weave_and_present(struct comp_vk_native_compositor *c,
                        bool is_repaint,
                        bool zero_copy,
                        uint64_t zc_image_u64,
                        uint64_t zc_view_u64,
                        int32_t zc_format,
                        uint32_t zc_width,
                        uint32_t zc_height,
                        uint32_t tgt_width,
                        uint32_t tgt_height,
                        bool ftime,
                        uint64_t *fp,
                        bool *out_skip_frame)
{
	struct vk_bundle *vk = &c->vk;
	xrt_result_t xret = XRT_SUCCESS;

	/*
	 * #868: every submit and the present below go to THIS queue. The app frame
	 * uses the app's queue (the one it handed us); a repaint uses the
	 * runtime-owned one where the driver gave us one. They must never be the
	 * same object UNSERIALIZED: a VkQueue is externally synchronised, and the
	 * runtime cannot serialise the app's own submits from outside the call —
	 * sharing it bare is undefined behaviour, measured as
	 * VK_ERROR_DEVICE_LOST.
	 *
	 * #902 shared-queue tier: on single-graphics-queue GPUs (Intel iGPUs,
	 * AMD) there IS no runtime-owned queue — the repaint then submits on the
	 * app's queue, made safe by VK_LAYER_DXR_queue_lock serializing every
	 * vkQueue* call per-queue INSIDE the call, app's own submits included.
	 * The repaint loop only starts in this mode after resolving the layer's
	 * marker on the device (see repaint.shared_queue).
	 */
	VkQueue queue =
	    (is_repaint && c->repaint_queue != VK_NULL_HANDLE) ? c->repaint_queue : vk->main_queue->queue;

	// Re-sync the output surface against the live ANativeWindow (Android).
	// On background→card the SurfaceView's surface is destroyed; presenting
	// to that dead window wedges the render thread (Adreno never reports
	// VK_ERROR_OUT_OF_DATE_KHR there) and the OS freezes the process. Skip
	// acquire/present entirely while no surface exists, and pick up the new
	// surface on resume. No-op on non-Android. #507
	enum comp_vk_native_target_surface_state sstate =
	    comp_vk_native_target_sync_surface(c->target);
	if (sstate == COMP_VK_NATIVE_TARGET_SURFACE_LOST) {
		*out_skip_frame = true;
		return XRT_SUCCESS;
	}

	uint32_t target_index;
	xret = comp_vk_native_target_acquire(c->target, &target_index, queue, is_repaint);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to acquire target");
		return xret;
	}

	/*
	 * #602: the acquire above may have rebuilt the swapchain itself
	 * (VK_ERROR_OUT_OF_DATE_KHR) — a recreate begin_frame never sees, and so
	 * never told the DP about. Catch every generation change here, whichever
	 * path produced it, BEFORE anything is recorded against the new images.
	 * See dp_notified_target_generation for what a missed notification costs.
	 *
	 * Safe to destroy DP objects at this point: both recreate paths drain the
	 * device, and this runs under the compositor lock on both callers.
	 */
	{
		uint32_t tgen = comp_vk_native_target_get_generation(c->target);
		if (tgen != c->dp_notified_target_generation) {
			c->dp_notified_target_generation = tgen;
			if (c->display_processor != NULL) {
				xrt_display_processor_vk_notify_target_recreated(
				    (struct xrt_display_processor_vk *)c->display_processor, tgen);
			}
		}
	}

	/*
	 * #911: that same acquire-side recreate can come back at a DIFFERENT SIZE
	 * — entering or leaving fullscreen is exactly that — and BOTH callers
	 * sampled tgt_width/tgt_height before calling us. Everything below is
	 * sized from that pair, including this frame's target framebuffer.
	 *
	 * Building a 3840x2160 framebuffer over a freshly created 1280x720
	 * attachment is not a cosmetic mismatch, it is a hard GPU fault: the next
	 * vkQueueSubmit returns VK_ERROR_DEVICE_LOST and the app spins in the
	 * failure path forever with the last frame frozen on the panel. Validation
	 * names it exactly — VUID-VkFramebufferCreateInfo-flags-04533/04534,
	 * "attachment has width 1280 smaller than the corresponding framebuffer
	 * width 3840" — immediately before the device dies.
	 *
	 * Once the acquire has returned, the target is the only authority on its
	 * own size, so re-read it. The layout/atlas state derived from the old
	 * size is stale for this one frame and the DP stretches over the
	 * difference; the next frame recomputes it. One slightly-wrong frame beats
	 * a lost device.
	 */
	if (c->target != NULL) {
		uint32_t live_w = 0, live_h = 0;
		comp_vk_native_target_get_dimensions(c->target, &live_w, &live_h);
		if (live_w != 0 && live_h != 0 && (live_w != tgt_width || live_h != tgt_height)) {
			static bool resize_logged = false;
			if (!resize_logged) {
				resize_logged = true;
				U_LOG_W("#911: target resized under the weave (%ux%u -> %ux%u) — the "
				        "acquire recreated the swapchain after this frame was sized; "
				        "using the live dimensions",
				        tgt_width, tgt_height, live_w, live_h);
			}
			tgt_width = live_w;
			tgt_height = live_h;
		}
	}

	// #868: a repaint records from its OWN pool — see repaint_cmd_pool.
	VkCommandPool cmd_pool = is_repaint && c->repaint_cmd_pool != VK_NULL_HANDLE
	                             ? c->repaint_cmd_pool
	                             : (VkCommandPool)(uintptr_t)comp_vk_native_renderer_get_cmd_pool(c->renderer);

	VkCommandBufferAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = cmd_pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};

	VkCommandBuffer cmd;
	VkFramebuffer target_fb = VK_NULL_HANDLE;
	VkResult res = vk->vkAllocateCommandBuffers(vk->device, &alloc_info, &cmd);
	if (res == VK_SUCCESS) {
		VkCommandBufferBeginInfo begin_info = {
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		vk->vkBeginCommandBuffer(cmd, &begin_info);

		uint64_t target_image, target_view;
		comp_vk_native_target_get_current_image(c->target, &target_image, &target_view);
		{
			// #879: name the images WE own, so the VkImage handle in a validation
			// error can be ATTRIBUTED rather than assumed. The same correlation
			// identified the queue in #868, and the assumption that
			// oldLayout-01197 concerns the TARGET has never been checked — the
			// render pass the DP actually exposes ends in PRESENT_SRC_KHR, so the
			// runtime's assumption may well be right and the offending image
			// something else entirely.
			static int img_logged = 0;
			if (img_logged < 3) {
				img_logged++;
				U_LOG_W("#879: target image=%p weave_scratch=%p local2d_scratch=%p",
				        (void *)(uintptr_t)target_image, (void *)c->weave_scratch,
				        (void *)c->local2d_scratch);
			}
		}

		// Display processor weaving path: record interlacing commands
		// into our command buffer using a framebuffer from our target.
		// This matches the multi-compositor approach where the weaver is
		// a command recorder, not a standalone presenter.
		VkRenderPass dp_render_pass = (c->display_processor != NULL)
		    ? xrt_display_processor_get_render_pass(c->display_processor)
		    : VK_NULL_HANDLE;

		// A self-submitting DP (e.g. the Leia CNSDK weaver) runs its
		// own render pass internally and exposes none to us, so
		// get_render_pass() is NULL. Route it through the DP path
		// anyway — gating solely on a non-NULL render pass would drop
		// it into the blit_to_target fallback below, which on Android
		// faults inside the Adreno driver (HW-3) and never weaves. Only
		// the explicit-cmd-buffer DP path needs our render pass/FB.
		bool dp_self_submits = (c->display_processor != NULL) &&
		    xrt_display_processor_is_self_submitting(c->display_processor);

		{
			// #879: which weave path is live decides what layout the target is
			// actually in after process_atlas, and therefore what the composite
			// and HUD must DECLARE. Never verified before.
			static bool dp_path_logged = false;
			if (!dp_path_logged) {
				dp_path_logged = true;
				U_LOG_W("#879: DP path — self_submits=%d render_pass=%s",
				        (int)dp_self_submits,
				        dp_render_pass != VK_NULL_HANDLE ? "present" : "NULL");
			}
		}
		if (c->display_processor != NULL &&
		    (dp_render_pass != VK_NULL_HANDLE || dp_self_submits)) {
			static bool dp_logged = false;
			if (!dp_logged) {
				U_LOG_W("VK rendering via display processor (compositor-owned swapchain)");
				dp_logged = true;
			}

			uint64_t src_image_u64, src_view_u64;
			int32_t view_format;
			uint32_t view_width, view_height, tc, tr;

			if (zero_copy) {
				src_image_u64 = zc_image_u64;
				src_view_u64 = zc_view_u64;
				view_format = zc_format;
			} else {
				src_image_u64 = comp_vk_native_renderer_get_atlas_image(c->renderer);
				src_view_u64 = comp_vk_native_renderer_get_atlas_view(c->renderer);
				view_format = comp_vk_native_renderer_get_format(c->renderer);
			}

			// #542: the DP and the window-space pass get the frame's
			// EFFECTIVE content layout — the grid the renderer painted
			// (== the mode layout for matched submissions).
			view_width = c->eff_layout.tile_w;
			view_height = c->eff_layout.tile_h;
			tc = c->eff_layout.cols;
			tr = c->eff_layout.rows;

			// Pre-weave: composite window-space layers per-tile INTO the
			// atlas. Skipped in zero-copy (no atlas) and when no
			// window-space layers exist.
			//
			// #868: also skipped on a repaint — it samples the APP's own
			// layer swapchain images, which the app has since reacquired
			// and redrawn. The atlas already carries what the last app
			// frame composited, so replaying the weave over it is both
			// correct and what the repaint is for.
			if (!zero_copy && !is_repaint) {
				uint32_t atlas_w_pre, atlas_h_pre;
				comp_vk_native_renderer_get_atlas_dimensions(c->renderer, &atlas_w_pre, &atlas_h_pre);
				vk_compositor_render_window_space_into_atlas(c, cmd,
				    (VkImage)(uintptr_t)src_image_u64,
				    (VkImageView)(uintptr_t)src_view_u64,
				    atlas_w_pre, atlas_h_pre,
				    view_width, view_height, tc, tr);
			}

			// Crop atlas to content dimensions before passing to DP
			{
				uint32_t content_w = tc * view_width;
				uint32_t content_h = tr * view_height;
				uint32_t atlas_w, atlas_h;
				if (zero_copy) {
					atlas_w = zc_width;
					atlas_h = zc_height;
				} else {
					comp_vk_native_renderer_get_atlas_dimensions(c->renderer, &atlas_w, &atlas_h);
				}
				vk_crop_atlas_for_dp(c, cmd, &src_image_u64, &src_view_u64,
				                      content_w, content_h, atlas_w, atlas_h);
			}

			// Create temporary framebuffer from the target's swapchain image.
			// Must use the DP's render pass for compatibility with vkCmdBeginRenderPass.
			// A self-submitting DP exposes no render pass, so leave target_fb
			// NULL for it (it weaves internally and ignores the FB) — calling
			// vkCreateFramebuffer with a NULL render pass would be invalid.
			if (dp_render_pass != VK_NULL_HANDLE) {
				VkImageView fb_view = (VkImageView)(uintptr_t)target_view;
				VkFramebufferCreateInfo fb_ci = {
				    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
				    .renderPass = dp_render_pass,
				    .attachmentCount = 1,
				    .pAttachments = &fb_view,
				    .width = tgt_width,
				    .height = tgt_height,
				    .layers = 1,
				};
				vk->vkCreateFramebuffer(vk->device, &fb_ci, NULL, &target_fb);
			}

			// #491 part 3 — flatten this frame's 2D-under layers PRE-weave
			// (into backdrop_scratch) and hand them to the DP so it
			// composites `backdrop over captured-desktop` under the 3D.
			// Recorded into `cmd`; the dp_self_submits flush below makes the
			// backdrop visible (SHADER_READ) before the DP's internal weave
			// samples it. Independent of target_image, so its order vs the
			// pre-weave target barrier is irrelevant. NULL ⟹ no under-layers.
			//
			// #868: the flatten samples the app's own Local2D swapchain
			// images, so a repaint MUST NOT re-run it — it reuses what the
			// last app frame produced. Re-reading them is exactly what made
			// the 2D bubble flicker on D3D11 and D3D12; see
			// [[repaint-never-touches-app-owned-state]].
			uint32_t bd_w = 0, bd_h = 0;
			VkImageView bd_view;
			if (is_repaint) {
				bd_view = (VkImageView)(uintptr_t)c->repaint.backdrop_view;
				bd_w = c->repaint.backdrop_w;
				bd_h = c->repaint.backdrop_h;
			} else {
				bd_view = vk_flatten_backdrop_2d(c, cmd, tgt_width, tgt_height, &bd_w, &bd_h);
				c->repaint.backdrop_view = (uint64_t)(uintptr_t)bd_view;
				c->repaint.backdrop_w = bd_w;
				c->repaint.backdrop_h = bd_h;
			}

			// #1073 — nothing app-supplied claimed the slot, so offer the
			// captured desktop instead. Deliberately also on a repaint: the
			// capture is compositor-owned state read off a socket, not the
			// app's Local2D swapchains, so re-running it breaks no #868 rule
			// and lets a repaint after a device rotation pick up the newly
			// re-cropped backdrop instead of re-presenting the stale one.
			if (bd_view == VK_NULL_HANDLE) {
				bd_view = vk_bg2d_backdrop(c, &bd_w, &bd_h);
			}

			// Pre-weave barrier: target → COLOR_ATTACHMENT_OPTIMAL
			VkImageMemoryBarrier pre_weave = {
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = 0,
			    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			    .image = (VkImage)(uintptr_t)target_image,
			    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};
			vk->vkCmdPipelineBarrier(cmd,
			    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			    0, 0, NULL, 0, NULL, 1, &pre_weave);

			if (ftime) {
				fp[1] = os_monotonic_get_ns();
			}
			if (dp_self_submits) {
				// Flush pre-DP work (WS-layer composite, atlas crop,
				// target-image layout transition) so the DP's
				// internal submit sees the right state on the GPU.
				vk->vkEndCommandBuffer(cmd);
				VkSubmitInfo pre_si = {
				    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				    .commandBufferCount = 1,
				    .pCommandBuffers = &cmd,
				};
				res = vk->vkQueueSubmit(queue, 1, &pre_si, VK_NULL_HANDLE);
				if (res == VK_SUCCESS) {
					vk->vkQueueWaitIdle(queue);
				}
			}

			if (ftime) {
				fp[2] = os_monotonic_get_ns();
			}
			// Hand the DP our lifecycle-managed view for this target
			// image. A self-submitting DP with no render pass (Leia
			// CNSDK) builds its own destination framebuffer and would
			// otherwise have to create its own VkImageView on the
			// swapchain image — which Adreno faults on. No-op for DPs
			// that don't expose the slot.
			xrt_display_processor_set_target_color_view(
			    c->display_processor, (VkImageView)(uintptr_t)target_view);

			// #491 part 3 — hand the DP this frame's backdrop (NULL ⟹ clears
			// it → desktop-only background). Must precede process_atlas.
			xrt_display_processor_set_background_2d(c->display_processor, bd_view, bd_w, bd_h);

			// Windowed weaving (runtime#757 / LeiaSR#85): anchor the lens phase
			// to the window's panel position. Must precede process_atlas.
			vk_update_present_origin(c);

			// Weave-latency harness mark (env-gated no-op otherwise). A
			// repaint paced itself unlocked and stays out of the #867
			// frame-cost ledger — it is not an app frame.
			if (is_repaint) {
				comp_vk_native_target_weave_mark_repaint(c->target, c->hardware_display_3d);
			} else {
				comp_vk_native_target_weave_mark(c->target, c->hardware_display_3d);
			}

			// Timing feedback: hand the DP last frame's MEASURED
			// weave→scanout residual so the vendor eye predictor
			// runs with an exact horizon (0 = unknown ⟹ DP heuristic).
			xrt_display_processor_vk_set_frame_timing(
			    (struct xrt_display_processor_vk *)c->display_processor,
			    comp_vk_native_target_get_measured_weave_ns(c->target),
			    (uint64_t)(U_TIME_1S_IN_NS / c->display_refresh_rate));

			// Call display processor with atlas (or zero-copy swapchain) texture.
			// The canvas sub-rect comes from vk_dp_canvas_rect() — the same
			// authority vk_bg2d_backdrop() cuts the compose-under backdrop to,
			// so the two can never disagree (#1101).
			const struct xrt_rect dp_canvas = vk_dp_canvas_rect(c);
			xrt_display_processor_process_atlas(
			    c->display_processor, dp_self_submits ? VK_NULL_HANDLE : cmd,
			    (VkImage_XDP)(uintptr_t)src_image_u64, (VkImageView)(uintptr_t)src_view_u64, view_width,
			    view_height, tc, tr, (VkFormat_XDP)view_format, target_fb,
			    (VkImage_XDP)(uintptr_t)target_image, tgt_width, tgt_height,
			    (VkFormat_XDP)comp_vk_native_target_get_format(c->target), dp_canvas.offset.w,
			    dp_canvas.offset.h, (uint32_t)dp_canvas.extent.w, (uint32_t)dp_canvas.extent.h);

			if (ftime) {
				fp[3] = os_monotonic_get_ns();
			}
			if (dp_self_submits) {
				// DP owned the post-weave submit. The vendor weave's
				// queue submit may still be in flight writing to
				// target_image; the HUD cmd buffer we're about to
				// record writes to the same image, so drain the GPU
				// before recording the HUD to avoid a target_image
				// race (audit B7). On Android the HUD is c->hud==NULL
				// and this overlay is a no-op, so the wait costs
				// nothing in practice — but keep it for safety when
				// HUD eventually wires up.
				vk->vkDeviceWaitIdle(vk->device);

				// Allocate a fresh cmd buffer for any post-DP
				// overlays (HUD), then fall through to the shared
				// end+submit below by re-using `cmd`.
				VkCommandBufferAllocateInfo post_ai = {
				    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				    .commandPool = cmd_pool,
				    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				    .commandBufferCount = 1,
				};
				vk->vkFreeCommandBuffers(vk->device, cmd_pool, 1, &cmd);
				vk->vkAllocateCommandBuffers(vk->device, &post_ai, &cmd);
				VkCommandBufferBeginInfo post_bi = {
				    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
				};
				vk->vkBeginCommandBuffer(cmd, &post_bi);
			}
			if (ftime) {
				fp[4] = os_monotonic_get_ns();
			}

			// Render pass finalLayout handles transition to PRESENT_SRC_KHR
			// Window-space layers are composed pre-weave into the atlas
			// (see vk_compositor_render_window_space_into_atlas), so we
			// only need the diagnostic HUD on the target post-weave.

			// #439 Phase 3 — overlay the frame's flattened 2D where the
			// zone mask says "2D" (M*weave + (1-M)*twod). No-op unless the
			// frame carries Local2D layers. Target stays PRESENT_SRC so the
			// HUD's own PRESENT_SRC→COLOR→PRESENT transition still applies.
			// #875: reuse the deposited 2D flatten on a repaint — see the
			// reuse_twod half of vk_composite_local_2d.
			/*
			 * #868 bisect: DXR_WEAVE_REPAINT_NO2D=1 makes a repaint skip the
			 * zones/Local2D composite entirely. Diagnostic only — it drops the 2D
			 * bands from repainted frames. It answers whether the zones-only
			 * VK_ERROR_DEVICE_LOST comes from this composite or from the weave
			 * around it. cube_handle_vk_win never calls this and is clean over
			 * 100 s, which is what makes it the prime suspect.
			 */
			static int no2d = -1;
			if (no2d < 0) {
				const char *ne = getenv("DXR_WEAVE_REPAINT_NO2D");
				no2d = (ne != NULL && ne[0] == '1') ? 1 : 0;
			}
			if (!(is_repaint && no2d == 1))
			vk_composite_local_2d(c, cmd, (VkImage)(uintptr_t)target_image,
			    (VkImageView)(uintptr_t)target_view, tgt_width, tgt_height,
			    /*
			     * Declare the layout the target ACTUALLY has, not the one the
			     * non-self-submitting path leaves behind.
			     *
			     * A self-submitting DP runs its own render pass whose
			     * finalLayout leaves the target in COLOR_ATTACHMENT_OPTIMAL —
			     * that is exactly why the explicit COLOR -> PRESENT_SRC barrier
			     * further down exists. Passing PRESENT_SRC_KHR here therefore
			     * declared a wrong oldLayout and tripped
			     * VUID-VkImageMemoryBarrier-oldLayout-01197 (measured: 20
			     * occurrences per run on cube_zones_vk_win). The composite must
			     * also LEAVE it in COLOR_ATTACHMENT_OPTIMAL so that barrier
			     * still has the layout it expects.
			     */
			    dp_self_submits ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
			                    : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			    dp_self_submits ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
			                    : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			    /*reuse_twod=*/is_repaint);

			// Diagnostic HUD overlay (TAB key toggle)
			vk_compositor_render_hud(c, cmd,
			    (VkImage)(uintptr_t)target_image, tgt_width, tgt_height,
			    dp_self_submits ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
			                    : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

			// A self-submitting DP (Leia CNSDK) ran its own internal
			// render pass, whose finalLayout leaves the target in
			// COLOR_ATTACHMENT_OPTIMAL — not PRESENT_SRC_KHR. The
			// non-self-submit path above relies on the compositor render
			// pass's finalLayout for that transition, but here there's no
			// such pass, so transition explicitly before present.
			if (dp_self_submits) {
				VkImageMemoryBarrier to_present = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				    .dstAccessMask = 0,
				    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				    .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
				    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				    .image = (VkImage)(uintptr_t)target_image,
				    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				};
				vk->vkCmdPipelineBarrier(cmd,
				    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
				    0, 0, NULL, 0, NULL, 1, &to_present);
			}
		} else {
			// No display processor (or mono/2D mode): blit atlas texture to target
			comp_vk_native_renderer_blit_to_target(c->renderer, cmd,
			                                        target_image, tgt_width, tgt_height);

			// Diagnostic HUD overlay (TAB key toggle)
			vk_compositor_render_hud(c, cmd,
			    (VkImage)(uintptr_t)target_image, tgt_width, tgt_height,
			    dp_self_submits ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
			                    : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		}

		vk->vkEndCommandBuffer(cmd);

		VkSubmitInfo submit_info = {
		    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		    .commandBufferCount = 1,
		    .pCommandBuffers = &cmd,
		};

		// #837: wait a fence scoped to THIS submit instead of draining
		// the whole queue — same wall time on the single-queue iGPU,
		// strictly narrower elsewhere, and the prerequisite for
		// deferring the present off the frame's critical path.
		// #868: a repaint signals its OWN fence — see repaint_fence.
		VkFence *fence_p = is_repaint ? &c->repaint_fence : &c->frame_fence;
		if (*fence_p == VK_NULL_HANDLE) {
			VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			vk->vkCreateFence(vk->device, &fci, NULL, fence_p);
		}
		res = vk->vkQueueSubmit(queue, 1, &submit_info, *fence_p);

		// Tell the DP the weave went to the GPU, and on which queue. The DP
		// RECORDED this weave (into `cmd`) but the submit is ours, so without
		// this it cannot know the frame is in flight — vendor late latching
		// needs exactly that edge to patch the vertex buffer of a queued frame
		// with a freshly predicted eye position. Must be the same queue we
		// submitted to: the vendor counts frames in flight with its own
		// fence-carrying submit on it.
		//
		// NOTE this is currently a no-op in effect, and deliberately still
		// called. We wait on `*fence_p` immediately below, so at most ONE frame
		// is ever in flight on this path and late latching has nothing queued
		// to patch. The call is wired now so the plumbing is correct and
		// testable the moment that synchronous wait is deferred (#837 calls
		// that out as its own goal). Wiring it later would mean discovering
		// this ordering constraint twice.
		if (res == VK_SUCCESS && c->display_processor != NULL) {
			xrt_display_processor_vk_weave_submitted(
			    (struct xrt_display_processor_vk *)c->display_processor, queue);
		}

		if (res == VK_SUCCESS) {
			if (*fence_p != VK_NULL_HANDLE) {
				vk->vkWaitForFences(vk->device, 1, fence_p, VK_TRUE, UINT64_MAX);
				vk->vkResetFences(vk->device, 1, fence_p);
			} else {
				vk->vkQueueWaitIdle(queue);
			}
		}

		vk->vkFreeCommandBuffers(vk->device, cmd_pool, 1, &cmd);
	}

	// Destroy temporary framebuffer after GPU is done
	if (target_fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, target_fb, NULL);
	}

	if (ftime) {
		fp[5] = os_monotonic_get_ns();
	}

	// Present
	xret = comp_vk_native_target_present(c->target, queue);

	/*
	 * VK-0 (#1178) — one-shot deposit proof, DXR_VK_DEPOSIT_PROBE=1 only.
	 *
	 * Deliberately AFTER the present: it takes the consumer's GPU-side fence
	 * wait for real and then reads the texture back once, so its single CPU
	 * map is off the frame's critical path. No-op with the probe env unset,
	 * with the deposit off, and on every non-Windows platform.
	 */
	comp_vk_deposit_probe_once(comp_vk_native_renderer_get_deposit(c->renderer), queue);

	if (ftime && fp[1] != 0) {
		fp[6] = os_monotonic_get_ns();
		vk_frame_timing_add(&s_ftiming, VK_FSTAGE_PRE, fp[0], fp[1]);
		vk_frame_timing_add(&s_ftiming, VK_FSTAGE_PREFLUSH, fp[1], fp[2]);
		vk_frame_timing_add(&s_ftiming, VK_FSTAGE_WEAVE, fp[2], fp[3]);
		vk_frame_timing_add(&s_ftiming, VK_FSTAGE_POSTWAIT, fp[3], fp[4]);
		vk_frame_timing_add(&s_ftiming, VK_FSTAGE_COMPOSITE, fp[4], fp[5]);
		vk_frame_timing_add(&s_ftiming, VK_FSTAGE_PRESENT, fp[5], fp[6]);
		vk_frame_timing_flush(&s_ftiming, fp[6]);
	}

#ifdef XRT_OS_WINDOWS
	if (c->owns_window && c->own_window != NULL) {
		comp_d3d11_window_signal_paint_done(c->own_window);
	}
#endif

	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to present");
		return xret;
	}

	return xret;
}

/*!
 * #868 repaint loop: re-weave the last atlas at panel rate while the app is
 * between frames, so the viewer's eye position keeps driving the interlace even
 * when the app cannot keep up. The weave is f(atlas, eye position), and the
 * display processor re-samples the eyes at weave time — so replaying the same
 * atlas is not a wasted frame, it is a fresher one.
 *
 * Fires only once the app has already MISSED a full refresh (>= 2 panel
 * periods). A tighter gate loses the race against the app's next frame, and the
 * repaint then just steals the lock from a submission that was about to happen
 * anyway.
 *
 * Paces itself with the lock RELEASED, then takes it for the whole replay, so
 * the queue and the vendor weaver only ever see one caller.
 */
static void *
vk_repaint_thread(void *ptr)
{
	struct comp_vk_native_compositor *c = (struct comp_vk_native_compositor *)ptr;

	while (os_thread_helper_is_running(&c->repaint_thread)) {
		const double hz = (c->display_refresh_rate > 1.0f) ? (double)c->display_refresh_rate : 60.0;
		const uint64_t period_ns = (uint64_t)(U_TIME_1S_IN_NS / hz);

		os_nanosleep((int64_t)(period_ns / 4));
		if (!os_thread_helper_is_running(&c->repaint_thread)) {
			break;
		}
		c->repaint.ticks++;

		// #868 diag: where the loop actually goes. A repaint that never fires
		// is indistinguishable from one that fires and produces nothing unless
		// the gate state is sampled — this is what caught the loop never
		// starting at all. Probe-only: at one line/second it is far too noisy
		// for the default path.
		if (c->repaint.force == 1 && (c->repaint.ticks % 240) == 0) {
			U_LOG_W("#868 vk repaint: ticks=%llu count=%llu armed=%d in_frame=%d "
			        "quiet_ns=%llu",
			        (unsigned long long)c->repaint.ticks, (unsigned long long)c->repaint.count,
			        (int)c->repaint.armed, (int)c->repaint.app_frame_in_progress,
			        (unsigned long long)(os_monotonic_get_ns() - c->repaint.last_app_frame_ns));
		}

		if (!c->repaint.armed || c->repaint.app_frame_in_progress) {
			continue;
		}
		// Keyed on the last APP frame, never on the last repaint — otherwise
		// repaints pace off their own timestamps and free-run.
		if (c->repaint.force != 1 &&
		    os_monotonic_get_ns() - c->repaint.last_app_frame_ns < period_ns * 2) {
			continue;
		}

		struct comp_vk_native_target *tgt = c->target;
		if (tgt == NULL) {
			continue;
		}

		// #868: the pacing below runs UNLOCKED (it blocks for up to a
		// panel period and must not stall the app's frame path), so a
		// recreate can land across it. Sample the image-set generation on
		// either side and drop the replay if it moved — every handle the
		// replay would use belongs to the generation sampled here.
		const uint32_t gen_before = comp_vk_native_target_get_generation(tgt);

		comp_vk_native_target_repaint_pace(tgt);

		os_mutex_lock(&c->mutex);

		// Re-check under the lock. app_frame_in_progress is load-bearing and
		// is NOT bypassed by the force probe: replaying into a half-written
		// layer_accum does not exercise the feature, it corrupts the frame.
		if (!os_thread_helper_is_running(&c->repaint_thread) || !c->repaint.armed ||
		    c->repaint.app_frame_in_progress || c->display_processor == NULL || c->target == NULL) {
			os_mutex_unlock(&c->mutex);
			continue;
		}
		if (comp_vk_native_target_get_generation(c->target) != gen_before) {
			// Resized/recreated across the pace. begin_frame's disarm
			// normally catches this first; the acquire-side out-of-date
			// recreate does not go through begin_frame, so this is the
			// path that catches that one.
			os_mutex_unlock(&c->mutex);
			continue;
		}
		if (c->repaint.force != 1 &&
		    os_monotonic_get_ns() - c->repaint.last_app_frame_ns < period_ns) {
			os_mutex_unlock(&c->mutex);
			continue;
		}

		uint32_t tgt_width = 0, tgt_height = 0;
		comp_vk_native_target_get_dimensions(c->target, &tgt_width, &tgt_height);

		/*
		 * A repaint is a FRAME as far as per-frame GPU resources are
		 * concerned, so it must re-arm them exactly like the top of
		 * layer_commit does.
		 *
		 * The Local2D descriptor pool is reset at most once per frame
		 * (vk_local2d_begin_frame_once) and this guard is what makes that
		 * "once" work. A repaint that leaves the guard set keeps ALLOCATING
		 * sets from a pool nobody resets — it exhausts after a few dozen
		 * replays and the allocation fails hard. That was the zones crash:
		 * cube_handle_vk_win survived only because with no zones/Local2D the
		 * composite early-returns and allocates nothing.
		 *
		 * Resetting here is safe: the frame path waits its fence before
		 * presenting, so no app command buffer is still referencing these sets
		 * by the time a repaint holds the lock.
		 */
		c->local2d_pool_reset_this_frame = false;

		/*
		 * #868 probe: DXR_WEAVE_REPAINT_DRAIN=1 drains the WHOLE DEVICE before
		 * and after the replay, which orders the app's queue against the
		 * repaint queue.
		 *
		 * Diagnostic only — vkDeviceWaitIdle every repaint is far too heavy to
		 * ship. It exists to answer one question: is the zones device-loss a
		 * cross-queue race on the images the two queues now share
		 * (local2d_scratch, weave_scratch, the mask rasters)? Those were
		 * implicitly ordered while everything ran on one queue; with a
		 * dedicated weaving queue nothing orders them, and vkQueueWaitIdle
		 * drains only the queue it is handed. If draining makes the loss go
		 * away, the fix is real cross-queue synchronisation on those images.
		 */
		static int drain = -1;
		if (drain < 0) {
			const char *de = getenv("DXR_WEAVE_REPAINT_DRAIN");
			drain = (de != NULL && de[0] == '1') ? 1 : 0;
		}
		if (drain == 1) {
			c->vk.vkDeviceWaitIdle(c->vk.device);
		}

		uint64_t fp[8] = {0};
		bool skip_frame = false;
		// zero_copy is hard false: c->repaint.armed is only set off that path.
		vk_dp_weave_and_present(c, /*is_repaint=*/true, /*zero_copy=*/false, 0, 0, 0, 0, 0,
		                        tgt_width, tgt_height, /*ftime=*/false, fp, &skip_frame);

		if (drain == 1) {
			c->vk.vkDeviceWaitIdle(c->vk.device);
		}

		c->repaint.count++;
		os_mutex_unlock(&c->mutex);

		static bool logged = false;
		if (!logged) {
			logged = true;
			U_LOG_W("#868: repainting last atlas at %.1f Hz while the app is between frames "
			        "(set DXR_WEAVE_REPAINT=0 to disable)",
			        hz);
		}
	}

	return NULL;
}

/*!
 * The frame path proper. Runs with c->mutex HELD — see the locking wrapper
 * vk_compositor_layer_commit below. Keeps its several early returns, which is
 * exactly why the lock lives in the wrapper rather than being taken here.
 */
static xrt_result_t
vk_compositor_layer_commit_locked(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;

	// #837 frame-stage timing (env-gated) — see vk_frame_timing above. fp[]
	// marks are taken at the windowed-DP path's stage boundaries.
	const bool ftime = debug_get_bool_option_frame_stage_timing();
	uint64_t fp[7] = {0};
	if (ftime) {
		fp[0] = os_monotonic_get_ns();
	}

	// Capture-intent poll — checks MCP request + trigger files once per
	// frame; consumed at the projection-done boundary or end of frame
	// depending on requested mode. See u_capture_intent.h.
	u_capture_intent_poll(&c->capture_intent, &c->mcp_capture);

	// Composite-tap state is per-frame: only valid when this frame's
	// Local2D composite runs and re-stashes it.
	c->tap_target_image = VK_NULL_HANDLE;

	// #439 Phase 3 — per-frame Local2D accumulator flag (read by
	// vk_effective_canvas + vk_composite_local_2d). Set once here so it reflects
	// this frame's committed layers. XR_DXR_display_zones: the zones-frame
	// flag is resolved in the same scan (one coherent per-frame decision).
	c->local_2d_last_frame = false;
	c->zones_frame = false;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		if (c->layer_accum.layers[i].data.type == XRT_LAYER_LOCAL_2D) {
			c->local_2d_last_frame = true;
		} else if (c->layer_accum.layers[i].data.type == XRT_LAYER_ZONE_3D) {
			c->zones_frame = true;
		}
	}

	// XR_DXR_display_zones hardware leg (P4). Zone-capable DP: the per-frame
	// wish publish at the end of this commit drives the per-region switch —
	// skip the global fallback. Legacy DP (no zone slots): tier-1 fallback —
	// "any zone active => request 3D" once on the rising edge, no forced 2D
	// on the falling edge.
	if (c->zones_frame && !c->zones_mode_requested && !vk_zone_dp_supported(c)) {
		c->zones_mode_requested = true;
		comp_vk_native_compositor_request_display_mode(&c->base.base, true);
	} else if (!c->zones_frame) {
		c->zones_mode_requested = false;
	}

	// Reset this frame's resolved wish view — vk_composite_local_2d sets it
	// in zones frames; a stale view from an earlier frame must never publish.
	// (zone_wish_w/h intentionally persist: they are the previous raster's
	// dims for the auto-wish seq dirty-check.)
	c->zone_wish_view = VK_NULL_HANDLE;

	// #491 part 3 — the Local2D descriptor pool is reset on first use this
	// frame (vk_local2d_begin_frame_once); both the pre-weave backdrop flatten
	// and the post-weave overlay composite share it. Reset the per-frame guard.
	c->local2d_pool_reset_this_frame = false;

	// Phase 1 diagnostic — env-gated per-client commit interval. Mirrors
	// the same `[CLIENT_FRAME_NS]` line emitted by the D3D11 in-process and
	// service compositors so per-app frame rates are directly comparable
	// across graphics APIs in standalone runs. One client per process here
	// so a static cache is fine; the compositor pointer tags the line.
	{
		static int log_client_frame_ns = -1;
		if (log_client_frame_ns < 0) {
			const char *e = getenv("DISPLAYXR_LOG_PRESENT_NS");
			log_client_frame_ns = (e != NULL && e[0] == '1') ? 1 : 0;
		}
		if (log_client_frame_ns) {
			static int64_t last_commit_ns = 0;
			int64_t now_ns = (int64_t)os_monotonic_get_ns();
			if (last_commit_ns != 0) {
				U_LOG_W("[CLIENT_FRAME_NS] client=%p dt_ns=%lld",
				        (void *)c,
				        (long long)(now_ns - last_commit_ns));
			}
			last_commit_ns = now_ns;
		}
	}

	// Get predicted eye positions
	struct xrt_vec3 left_eye = {-0.032f, 0.0f, 0.6f};
	struct xrt_vec3 right_eye = {0.032f, 0.0f, 0.6f};

	if (c->display_processor != NULL) {
		struct xrt_eye_positions eyes;
		if (xrt_display_processor_get_predicted_eye_positions(c->display_processor, &eyes) &&
		    eyes.valid) {
			left_eye.x = eyes.eyes[0].x;
			left_eye.y = eyes.eyes[0].y;
			left_eye.z = eyes.eyes[0].z;
			right_eye.x = eyes.eyes[1].x;
			right_eye.y = eyes.eyes[1].y;
			right_eye.z = eyes.eyes[1].z;
		}
	}

	// Sync hardware_display_3d, tile layout, and per-view dimensions
	// from device's active rendering mode.
	// Legacy apps: view dims are fixed at compromise scale, only update tile layout.
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			const struct xrt_rendering_mode *mode = &c->xdev->rendering_modes[idx];
			c->hardware_display_3d = mode->hardware_display_3d;
			if (mode->tile_columns > 0 && c->renderer != NULL) {
				if (!c->legacy_app_tile_scaling) {
					// Extension app: sync view dimensions from active mode
					uint32_t new_vw = mode->view_width_pixels;
					uint32_t new_vh = mode->view_height_pixels;
					uint32_t new_aw = mode->atlas_width_pixels;
					uint32_t new_ah = mode->atlas_height_pixels;
#if defined(XRT_OS_WINDOWS) || defined(__APPLE__) || defined(XRT_OS_LINUX_DESKTOP)
					if (!c->owns_window && c->settings.preferred.width > 0 &&
					    c->settings.preferred.height > 0) {
						// Handle app: window may be smaller than the display,
						// so scale view dims to the actual window client area
						// (matches the D3D11/D3D12 path) — keeps the atlas
						// content region, DP input, and atlas capture at window
						// resolution.
						u_tiling_compute_canvas_view(mode, c->settings.preferred.width,
						                             c->settings.preferred.height,
						                             &new_vw, &new_vh);
					}
#endif
					if (new_vw > 0 && new_vh > 0) {
						comp_vk_native_renderer_resize(
						    c->renderer, new_vw, new_vh, new_aw, new_ah);
					}
				}
				// Always update tile layout (both legacy and extension apps)
				comp_vk_native_renderer_set_tile_layout(
				    c->renderer, mode->tile_columns, mode->tile_rows);
			}
		}
	}

	// Runtime-side 2D/3D toggle from qwerty V key
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (c->xsysd != NULL) {
		bool force_2d = false;
		bool toggled = qwerty_check_display_mode_toggle(c->xsysd->xdevs, c->xsysd->xdev_count, &force_2d);
		if (toggled) {
			struct xrt_device *head = c->xsysd->static_roles.head;
			if (head != NULL && head->hmd != NULL) {
				if (force_2d) {
					uint32_t cur = head->hmd->active_rendering_mode_index;
					if (cur < head->rendering_mode_count &&
					    head->rendering_modes[cur].hardware_display_3d) {
						c->last_3d_mode_index = cur;
					}
					head->hmd->active_rendering_mode_index = 0;
				} else {
					head->hmd->active_rendering_mode_index = c->last_3d_mode_index;
				}
			}
			comp_vk_native_compositor_request_display_mode(&c->base.base, !force_2d);
		}

		// Rendering mode change from qwerty 0/1/2/3/4 keys.
		// Legacy apps only support V toggle — skip direct mode selection.
		if (!c->legacy_app_tile_scaling) {
			int render_mode = -1;
			if (qwerty_check_rendering_mode_change(c->xsysd->xdevs, c->xsysd->xdev_count, &render_mode)) {
				struct xrt_device *head = c->xsysd->static_roles.head;
				if (head != NULL) {
					xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, render_mode);
				}
			}
		}
	}
#endif

	// Get target dimensions
	uint32_t tgt_width = c->settings.preferred.width;
	uint32_t tgt_height = c->settings.preferred.height;
	if (c->target != NULL) {
		comp_vk_native_target_get_dimensions(c->target, &tgt_width, &tgt_height);
	}

	// Per-frame effective CONTENT layout (#542): tile grid/dims from the
	// SUBMISSION, decoupled from the hardware weave-state. Feeds the
	// renderer draw, the window-space pass, both DP handoffs, and the
	// capture path — they must all agree on the frame's geometry.
	vk_compute_effective_layout(c);

	// Zero-copy check: can we pass the app's swapchain directly to the DP?
	bool zero_copy = false;
	uint64_t zc_image_u64 = 0;
	uint64_t zc_view_u64 = 0;
	int32_t zc_format = 0;
	uint32_t zc_width = 0, zc_height = 0;

	{
		const struct xrt_rendering_mode *mode = NULL;
		if (c->xdev != NULL && c->xdev->hmd != NULL) {
			uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
			if (idx < c->xdev->rendering_mode_count)
				mode = &c->xdev->rendering_modes[idx];
		}
		if (mode != NULL && c->layer_accum.layer_count == 1) {
			struct comp_layer *layer = &c->layer_accum.layers[0];
			if (layer->data.type == XRT_LAYER_PROJECTION ||
			    layer->data.type == XRT_LAYER_PROJECTION_DEPTH) {
				uint32_t vc = mode->view_count;
				// #542: a hardware/content divergence frame (submitted
				// views != mode views) must take the atlas path — the
				// per-view loops below would read stale proj.v[] slots,
				// and zero-copy can't re-tile a mismatched submission.
				bool same_sc = (vc > 0 && vc <= XRT_MAX_VIEWS && layer->data.view_count == vc &&
				                layer->sc_array[0] != NULL);
				for (uint32_t v = 1; v < vc && same_sc; v++) {
					if (layer->sc_array[v] != layer->sc_array[0])
						same_sc = false;
				}
				if (same_sc) {
					uint32_t img_idx = layer->data.proj.v[0].sub.image_index;
					bool same_idx = true;
					for (uint32_t v = 1; v < vc; v++) {
						if (layer->data.proj.v[v].sub.image_index != img_idx) {
							same_idx = false;
							break;
						}
					}
					bool all_array_zero = same_idx;
					for (uint32_t v = 0; v < vc && all_array_zero; v++) {
						if (layer->data.proj.v[v].sub.array_index != 0)
							all_array_zero = false;
					}
					if (all_array_zero) {
						uint32_t sw, sh;
						comp_vk_native_swapchain_get_dimensions(layer->sc_array[0], &sw, &sh);
						int32_t rxs[XRT_MAX_VIEWS], rys[XRT_MAX_VIEWS];
						uint32_t rws[XRT_MAX_VIEWS], rhs_arr[XRT_MAX_VIEWS];
						for (uint32_t v = 0; v < vc; v++) {
							rxs[v] = layer->data.proj.v[v].sub.rect.offset.w;
							rys[v] = layer->data.proj.v[v].sub.rect.offset.h;
							rws[v] = layer->data.proj.v[v].sub.rect.extent.w;
							rhs_arr[v] = layer->data.proj.v[v].sub.rect.extent.h;
						}
						if (u_tiling_can_zero_copy(vc, rxs, rys, rws, rhs_arr, sw, sh, mode)) {
							zc_image_u64 = comp_vk_native_swapchain_get_image(layer->sc_array[0], img_idx);
							zc_view_u64 = comp_vk_native_swapchain_get_image_view(layer->sc_array[0], img_idx);
							if (zc_image_u64 != 0 && zc_view_u64 != 0) {
								zero_copy = true;
								zc_format = comp_vk_native_renderer_get_format(c->renderer);
								zc_width = sw;
								zc_height = sh;
							}
						}
					}
				}
			}
		}
	}

	// Record the frame's effective capture source = exactly what the DP will
	// receive (renderer atlas, or the zero-copy app swapchain). The atlas
	// capture reads this so it shows the DP's input regardless of render mode
	// — a zero-copy frame otherwise captures the unpainted renderer atlas.
	if (zero_copy) {
		c->capture_src_image_u64 = zc_image_u64;
		c->capture_src_format = zc_format;
		c->capture_src_atlas_w = zc_width;
		c->capture_src_atlas_h = zc_height;
	} else {
		c->capture_src_image_u64 = comp_vk_native_renderer_get_atlas_image(c->renderer);
		c->capture_src_format = comp_vk_native_renderer_get_format(c->renderer);
		comp_vk_native_renderer_get_atlas_dimensions(c->renderer, &c->capture_src_atlas_w,
		                                             &c->capture_src_atlas_h);
	}

	// Render projection layers to atlas texture (skip if zero-copy)
	xrt_result_t xret = XRT_SUCCESS;
	if (!zero_copy) {
		xret = comp_vk_native_renderer_draw(
		    c->renderer, &c->layer_accum, &left_eye, &right_eye, tgt_width, tgt_height, &c->eff_layout);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to render layers");
			return xret;
		}

		// Projection-only capture point — atlas now contains projection
		// layers across all tiles; window-space layers haven't been
		// composed in yet. Atlas is in SHADER_READ_ONLY_OPTIMAL after
		// renderer's terminating barrier (see comp_vk_native_renderer.c).
		// Skipped in zero-copy mode (no renderer atlas to read).
		vk_native_dispatch_capture(c, MCP_CAPTURE_MODE_PROJECTION_ONLY);
	}

	// Shared texture output path — render to IOSurface-backed VkImage
	if (c->has_shared_texture && c->shared_image != VK_NULL_HANDLE) {
		VkCommandPool cmd_pool = (VkCommandPool)(uintptr_t)
		    comp_vk_native_renderer_get_cmd_pool(c->renderer);

		VkCommandBufferAllocateInfo cmd_alloc = {
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		    .commandPool = cmd_pool,
		    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		    .commandBufferCount = 1,
		};

		VkCommandBuffer cmd;
		VkResult res = vk->vkAllocateCommandBuffers(vk->device, &cmd_alloc, &cmd);
		if (res != VK_SUCCESS) {
			U_LOG_E("Failed to allocate command buffer for shared texture");
			return XRT_ERROR_VULKAN;
		}

		VkCommandBufferBeginInfo begin_info = {
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		vk->vkBeginCommandBuffer(cmd, &begin_info);

		bool weaving_done = false;

		// Display processor path (weaving for 3D, passthrough for 2D)
		if (c->display_processor != NULL) {
			uint64_t src_image_u64, src_view_u64;
			int32_t view_format;
			uint32_t view_width, view_height, tc, tr;

			if (zero_copy) {
				src_image_u64 = zc_image_u64;
				src_view_u64 = zc_view_u64;
				view_format = zc_format;
			} else {
				src_image_u64 = comp_vk_native_renderer_get_atlas_image(c->renderer);
				src_view_u64 = comp_vk_native_renderer_get_atlas_view(c->renderer);
				view_format = comp_vk_native_renderer_get_format(c->renderer);
			}

			// #542: the DP and the window-space pass get the frame's
			// EFFECTIVE content layout — the grid the renderer painted
			// (== the mode layout for matched submissions).
			view_width = c->eff_layout.tile_w;
			view_height = c->eff_layout.tile_h;
			tc = c->eff_layout.cols;
			tr = c->eff_layout.rows;

			// Pre-weave: composite window-space layers per-tile INTO the atlas
			// so the DP weaves them along with the projection content. Skipped
			// in zero-copy (no atlas) and when no window-space layers exist.
			if (!zero_copy) {
				uint32_t atlas_w_pre, atlas_h_pre;
				comp_vk_native_renderer_get_atlas_dimensions(c->renderer, &atlas_w_pre, &atlas_h_pre);
				vk_compositor_render_window_space_into_atlas(c, cmd,
				    (VkImage)(uintptr_t)src_image_u64,
				    (VkImageView)(uintptr_t)src_view_u64,
				    atlas_w_pre, atlas_h_pre,
				    view_width, view_height, tc, tr);
			}

			// Crop atlas to content dimensions before passing to DP
			{
				uint32_t content_w = tc * view_width;
				uint32_t content_h = tr * view_height;
				uint32_t atlas_w, atlas_h;
				if (zero_copy) {
					atlas_w = zc_width;
					atlas_h = zc_height;
				} else {
					comp_vk_native_renderer_get_atlas_dimensions(c->renderer, &atlas_w, &atlas_h);
				}
				vk_crop_atlas_for_dp(c, cmd, &src_image_u64, &src_view_u64,
				                      content_w, content_h, atlas_w, atlas_h);
			}

			VkRenderPass dp_render_pass = xrt_display_processor_get_render_pass(c->display_processor);
			VkFramebuffer shared_fb = VK_NULL_HANDLE;
			if (dp_render_pass != VK_NULL_HANDLE) {
				VkFramebufferCreateInfo fb_ci = {
				    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
				    .renderPass = dp_render_pass,
				    .attachmentCount = 1,
				    .pAttachments = &c->shared_view,
				    .width = tgt_width,
				    .height = tgt_height,
				    .layers = 1,
				};
				vk->vkCreateFramebuffer(vk->device, &fb_ci, NULL, &shared_fb);
			}

			uint32_t dp_target_w = tgt_width;
			uint32_t dp_target_h = tgt_height;

			// #491 part 3 — flatten this frame's 2D-under layers PRE-weave (into
			// backdrop_scratch) and hand them to the DP so it composites
			// `backdrop over captured-desktop` under the 3D. Recorded into `cmd`
			// here; the dp_self_submits flush below makes the backdrop visible
			// (in SHADER_READ) before the DP's internal weave samples it. On the
			// non-self-submit path the flatten's SHADER_READ barrier orders it
			// within the one submit. VK_NULL_HANDLE ⟹ no under-layers this frame.
			uint32_t bd_w = 0, bd_h = 0;
			VkImageView bd_view = vk_flatten_backdrop_2d(c, cmd, dp_target_w, dp_target_h, &bd_w, &bd_h);

			// #1073 — same precedence as the window path: the captured desktop
			// only fills the slot the app's own Local2D backdrop declined. The
			// upload is self-contained (its own one-shot command buffer, waited
			// on), so it is complete before the flush below either way.
			if (bd_view == VK_NULL_HANDLE) {
				bd_view = vk_bg2d_backdrop(c, &bd_w, &bd_h);
			}

			bool dp_self_submits =
			    xrt_display_processor_is_self_submitting(c->display_processor);

			if (dp_self_submits) {
				// Flush pre-DP work (window-space composite, atlas crop,
				// 2D-under backdrop flatten) so the DP's internal submit sees a
				// coherent atlas + backdrop.
				vk->vkEndCommandBuffer(cmd);
				VkSubmitInfo pre_si = {
				    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				    .commandBufferCount = 1,
				    .pCommandBuffers = &cmd,
				};
				res = vk->vkQueueSubmit(vk->main_queue->queue, 1, &pre_si, VK_NULL_HANDLE);
				if (res == VK_SUCCESS) {
					vk->vkQueueWaitIdle(vk->main_queue->queue);
				}
			}

			// Hand the DP this frame's backdrop (NULL ⟹ clears it → desktop-only
			// background). Must precede process_atlas.
			xrt_display_processor_set_background_2d(c->display_processor, bd_view, bd_w, bd_h);

			// Windowed weaving (runtime#757 / LeiaSR#85): anchor the lens phase to
			// the window's panel position. Must precede process_atlas.
			vk_update_present_origin(c);

			// Same single canvas authority as the window path (#1101).
			const struct xrt_rect dp_canvas = vk_dp_canvas_rect(c);
			xrt_display_processor_process_atlas(
			    c->display_processor, dp_self_submits ? VK_NULL_HANDLE : cmd,
			    (VkImage_XDP)(uintptr_t)src_image_u64, (VkImageView)(uintptr_t)src_view_u64, view_width,
			    view_height, tc, tr, (VkFormat_XDP)view_format, shared_fb, (VkImage_XDP)c->shared_image,
			    dp_target_w, dp_target_h, (VkFormat_XDP)view_format, dp_canvas.offset.w, dp_canvas.offset.h,
			    (uint32_t)dp_canvas.extent.w, (uint32_t)dp_canvas.extent.h);

			if (dp_self_submits) {
				// DP owns its own submit — nothing left for us to do this
				// frame except mark woven and clean up.
				weaving_done = true;
			} else {
				vk->vkEndCommandBuffer(cmd);

				VkSubmitInfo submit_info = {
				    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				    .commandBufferCount = 1,
				    .pCommandBuffers = &cmd,
				};
				res = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit_info, VK_NULL_HANDLE);
				if (res == VK_SUCCESS) {
					vk->vkQueueWaitIdle(vk->main_queue->queue);
					weaving_done = true;
				}
			}

			if (shared_fb != VK_NULL_HANDLE) {
				vk->vkDestroyFramebuffer(vk->device, shared_fb, NULL);
			}
		}

		// Fallback: blit atlas texture to shared image
		if (!weaving_done) {
			comp_vk_native_renderer_blit_to_shared(c->renderer, cmd,
			    (uint64_t)(uintptr_t)c->shared_image, tgt_width, tgt_height);

			vk->vkEndCommandBuffer(cmd);

			VkSubmitInfo submit_info = {
			    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			    .commandBufferCount = 1,
			    .pCommandBuffers = &cmd,
			};
			res = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit_info, VK_NULL_HANDLE);
			if (res == VK_SUCCESS) {
				vk->vkQueueWaitIdle(vk->main_queue->queue);
			}
		}

		vk->vkFreeCommandBuffers(vk->device, cmd_pool, 1, &cmd);

		// #224 / ADR-027 P4: sideband-sync this client's zone mask with
		// the DP (legacy sticky publish / clear edge; the zones wish only
		// exists where the composite ran — not on this shared path yet).
		vk_sync_zone_mask_to_dp(c);
		return XRT_SUCCESS;
	}

	// If we have a target (window), present to it
	if (c->target != NULL) {
		// #868: publish what the repaint thread needs to replay this weave.
		// Armed only off the zero-copy path: there the atlas IS the app's own
		// swapchain image, which it reacquires and overwrites, so replaying it
		// would weave whatever the app has drawn since.
		c->repaint.view_w = c->eff_layout.tile_w;
		c->repaint.view_h = c->eff_layout.tile_h;
		c->repaint.cols = c->eff_layout.cols;
		c->repaint.rows = c->eff_layout.rows;
		c->repaint.armed = !zero_copy;

		bool skip_frame = false;
		xret = vk_dp_weave_and_present(c, /*is_repaint=*/false, zero_copy, zc_image_u64,
		                               zc_view_u64, zc_format, zc_width, zc_height, tgt_width,
		                               tgt_height, ftime, fp, &skip_frame);
		if (skip_frame) {
			return XRT_SUCCESS;
		}
		if (xret != XRT_SUCCESS) {
			return xret;
		}

		// Only a REAL frame resets the quiet-gate. A repaint must not, or
		// repaints would pace off their own timestamps and free-run.
		c->repaint.last_app_frame_ns = os_monotonic_get_ns();
	}

	// #224 / ADR-027 P4: sideband-sync this client's zone state with the DP
	// — in zones frames this publishes the WISH the composite just resolved
	// (the queue submit + wait-idle above made its content GPU-complete);
	// in legacy frames the sticky submitted mask; the clear edge otherwise.
	vk_sync_zone_mask_to_dp(c);

	// Post-compose capture (#210) — captures the fully composed atlas
	// (projection + window-space + quads) the DP just received. Skipped
	// when intent is for projection-only (consumed earlier) or empty.
	vk_native_dispatch_capture(c, MCP_CAPTURE_MODE_POST_COMPOSE);

	// Composite-tap diagnostics (#833 debugging) — trigger-file dump of the
	// final target / weave snapshot / flattened 2D scratch.
	vk_native_dispatch_composite_tap(c);

	return XRT_SUCCESS;
}

/*!
 * #868 locking wrapper. Serialises the whole frame path against the repaint
 * replay, so the app's queue submits, the vendor weave and the present are
 * never concurrent with a repaint's.
 *
 * Clearing app_frame_in_progress here — under the lock, after the frame path
 * has returned — is what closes the submission window opened by layer_begin.
 * It is cleared unconditionally so an early return inside the frame path
 * cannot leak it and wedge the repaint loop off permanently.
 */
static xrt_result_t
vk_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

	os_mutex_lock(&c->mutex);
	xrt_result_t xret = vk_compositor_layer_commit_locked(xc, sync_handle);
	c->repaint.app_frame_in_progress = false;
	os_mutex_unlock(&c->mutex);

	return xret;
}

static xrt_result_t
vk_compositor_layer_commit_with_semaphore(struct xrt_compositor *xc,
                                           struct xrt_compositor_semaphore *xcsem,
                                           uint64_t value)
{
	return vk_compositor_layer_commit(xc, XRT_GRAPHICS_SYNC_HANDLE_INVALID);
}

static void
vk_compositor_destroy(struct xrt_compositor *xc)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;

	U_LOG_I("Destroying VK native compositor");

	// #868: stop the repaint loop FIRST. It touches the queue, the target and
	// the display processor, all of which are torn down below. Joining before
	// anything is released is what makes the rest of this function safe.
	os_thread_helper_destroy(&c->repaint_thread);

	// Uninstall MCP capture hook before any GPU resources go away — the
	// MCP thread can no longer post requests against us after this returns.
	mcp_capture_uninstall();
	mcp_capture_fini(&c->mcp_capture);

	vk->vkDeviceWaitIdle(vk->device);

	// Compose-under backdrop (#1073). The socket receiver behind it is
	// process-global and deliberately left running: one screen has one
	// background, and a producer is allowed to outlive any single session.
	comp_bg2d_teardown(&c->bg2d, vk);

	// Destroy HUD resources
	if (c->hud_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, c->hud_image, NULL);
	}
	if (c->hud_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, c->hud_memory, NULL);
	}
	u_hud_destroy(&c->hud);

	// Destroy window-space (HUD) alpha-blend pipeline + cached atlas FB
	if (c->atlas_ws_fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, c->atlas_ws_fb, NULL);
		c->atlas_ws_fb = VK_NULL_HANDLE;
	}
	vk_hud_blend_fini(&c->window_space_blend, vk);

	// #439 Phase 3 — masked composite pipelines + scratch images. (The active
	// zone mask is owned by the oxr handle, freed via zone_mask_destroy.)
	// #868: repaint-owned objects. The loop was joined at the top of destroy,
	// so nothing can still be recording or waiting on these.
	if (c->repaint_cmd_pool != VK_NULL_HANDLE) {
		vk->vkDestroyCommandPool(vk->device, c->repaint_cmd_pool, NULL);
		c->repaint_cmd_pool = VK_NULL_HANDLE;
	}
	if (c->repaint_fence != VK_NULL_HANDLE) {
		vk->vkDestroyFence(vk->device, c->repaint_fence, NULL);
		c->repaint_fence = VK_NULL_HANDLE;
	}

	if (c->frame_fence != VK_NULL_HANDLE) {
		c->vk.vkDestroyFence(c->vk.device, c->frame_fence, NULL);
		c->frame_fence = VK_NULL_HANDLE;
	}

	vk_release_local2d_state(c);

	// Destroy DP input crop image
	if (c->dp_input_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, c->dp_input_view, NULL);
	}
	if (c->dp_input_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, c->dp_input_image, NULL);
	}
	if (c->dp_input_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, c->dp_input_memory, NULL);
	}

	// Destroy display processor
	// #224 P4: withdraw this client's zone contribution from the vendor's
	// union before the DP goes away (clear-on-teardown edge).
	if (c->zone_published && c->display_processor != NULL) {
		xrt_display_processor_clear_local_zone_mask(c->display_processor);
		c->zone_published = false;
	}
	xrt_display_processor_destroy(&c->display_processor);

	// Destroy shared texture resources
	if (c->shared_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, c->shared_view, NULL);
	}
	if (c->shared_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
	}
	if (c->shared_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, c->shared_memory, NULL);
	}

	if (c->renderer != NULL) {
		comp_vk_native_renderer_destroy(&c->renderer);
	}

	if (c->target != NULL) {
		comp_vk_native_target_destroy(&c->target);
	}

#ifdef XRT_OS_WINDOWS
	if (c->owns_window && c->own_window != NULL) {
		comp_d3d11_window_destroy(&c->own_window);
	}
#endif

#ifdef XRT_OS_MACOS
	if (c->macos_window != NULL) {
		comp_vk_native_window_macos_destroy(&c->macos_window);
	}
#endif

#ifdef XRT_OS_LINUX_DESKTOP
	if (c->xcb_window != NULL) {
		comp_vk_native_window_xcb_destroy(&c->xcb_window);
	}
#ifdef DXR_HAVE_WL_GEOM
	if (c->wl_geom != NULL) {
		comp_vk_native_wl_geom_destroy(&c->wl_geom);
	}
#endif
#ifdef DXR_HAVE_DIRECT_SCANOUT
	// After the target (swapchain) is gone — the backend owns the surface it
	// borrowed to it — release the display back to the X server.
	if (c->direct_window != NULL) {
		comp_vk_native_window_direct_destroy(&c->direct_window);
	}
#endif
#endif

	// Destroy command pool (we created it for the display processor factory)
	if (c->cmd_pool != VK_NULL_HANDLE) {
		vk->vkDestroyCommandPool(vk->device, c->cmd_pool, NULL);
	}

	// Note: we do NOT destroy the VkDevice — it belongs to the app.
	// vk_bundle cleanup is minimal (just mutexes).

	// #868: last, after the repaint thread has been joined above.
	os_mutex_destroy(&c->mutex);

	free(c);
}

/*!
 * Sanity-check a display-processor's C vtable before the compositor trusts it.
 *
 * The DP is a plug-in-owned struct of function pointers. On Windows we have
 * observed (standalone VK + Leia SR) the DP's small heap block being reused by
 * the GPU driver's Vulkan shader compiler during pipeline creation, leaving the
 * vtable holding garbage (non-code) pointers — a subsequent call through it is a
 * hard ACCESS_VIOLATION. This validates that the load-bearing slots point into
 * committed, executable pages; NULL is left alone (it's a valid "unsupported"
 * sentinel that every xrt_display_processor_* wrapper already tolerates).
 *
 * Returns true if the vtable looks usable. On non-Windows this is a no-op
 * (returns true) — the failure mode is Windows-driver-specific.
 */
static bool
dp_vtable_looks_sane(struct xrt_display_processor *dp)
{
	if (dp == NULL) {
		return false;
	}
#ifdef XRT_OS_WINDOWS
	// Load-bearing slots the compositor calls. process_atlas + destroy are
	// mandatory; the rest are optional (may legitimately be NULL).
	void *fns[] = {
	    (void *)dp->process_atlas,        (void *)dp->destroy,
	    (void *)dp->get_display_pixel_info, (void *)dp->get_render_pass,
	    (void *)dp->get_display_dimensions, (void *)dp->set_eye_tracking_mode,
	};
	if (dp->process_atlas == NULL || dp->destroy == NULL) {
		return false; // mandatory slots
	}
	const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
		if (fns[i] == NULL) {
			continue; // valid "unsupported" sentinel
		}
		MEMORY_BASIC_INFORMATION mbi;
		if (VirtualQuery(fns[i], &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
		    (mbi.Protect & exec) == 0) {
			return false;
		}
	}
#endif
	return true;
}

#if defined(XRT_OS_MACOS) || defined(XRT_OS_LINUX_DESKTOP)
/*!
 * Manual override for the self-owned window position: DXR_WINDOW_POS="x,y"
 * (top-down desktop pixels), checked BEFORE the plug-in-reported display
 * position. For dev boxes and DPs with no real panel position — sim_display
 * reports (0, 0) = primary by convention. #715.
 */
static void
apply_window_pos_override(int32_t *left, int32_t *top)
{
	const char *e = getenv("DXR_WINDOW_POS");
	if (e == NULL || e[0] == '\0') {
		return;
	}
	int x = 0;
	int y = 0;
	if (sscanf(e, "%d,%d", &x, &y) == 2) {
		U_LOG_W("DXR_WINDOW_POS=%s overrides plug-in display position (%d, %d)", e, (int)*left, (int)*top);
		*left = (int32_t)x;
		*top = (int32_t)y;
	} else {
		U_LOG_W("DXR_WINDOW_POS='%s' malformed — expected 'x,y'; using plug-in display position", e);
	}
}
#endif


/*
 *
 * Exported functions
 *
 */

xrt_result_t
comp_vk_native_compositor_create(struct xrt_device *xdev,
                                 void *hwnd,
                                 bool window_is_wayland,
                                 void *vk_instance,
                                 void *vk_physical_device,
                                 void *vk_device,
                                 uint32_t queue_family_index,
                                 uint32_t queue_index,
                                 int32_t runtime_queue_family,
                                 int32_t runtime_queue_index,
                                 void *dp_factory_vk,
                                 void *shared_texture_handle,
                                 bool transparent_background,
                                 int32_t display_screen_left,
                                 int32_t display_screen_top,
                                 bool app_timeline_semaphores,
                                 struct xrt_compositor_native **out_xc)
{
	if (vk_device == NULL) {
		U_LOG_E("VkDevice is null");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	(void)window_is_wayland; // consumed only in the XRT_HAVE_WAYLAND Linux path

	U_LOG_I("Creating VK native compositor");

	struct comp_vk_native_compositor *c = U_TYPED_CALLOC(struct comp_vk_native_compositor);
	if (c == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	c->xdev = xdev;
	c->queue_family_index = queue_family_index;
	c->repaint_queue = VK_NULL_HANDLE;
	c->repaint_queue_family = runtime_queue_family;
	c->repaint_queue_index = runtime_queue_index;
	c->shared_texture_handle = shared_texture_handle;
	// VK-0 (#1178): only the deposit reads this. Deliberately NOT forwarded to
	// vk_init_from_given below — that call keeps saying `timeline_semaphore_enabled
	// = false`, which is the runtime declaring what IT uses the bundle for, and
	// changing it would move behaviour on the flag-off path.
	c->app_timeline_semaphores = app_timeline_semaphores;
	c->hardware_display_3d = true;
	c->last_3d_mode_index = 1;

	// #868: before ANY path that can reach vk_compositor_destroy — it both
	// joins the repaint thread and destroys this mutex, so they must be valid
	// even on the earliest create failure. os_thread_helper_init only prepares
	// the handle; the loop is started later, once the target exists.
	os_mutex_init(&c->mutex);
	os_thread_helper_init(&c->repaint_thread);

	// Initialize vk_bundle from the app's existing VkDevice
	VkResult vk_ret = vk_init_from_given(
	    &c->vk,
	    vkGetInstanceProcAddr,
	    (VkInstance)vk_instance,
	    (VkPhysicalDevice)vk_physical_device,
	    (VkDevice)vk_device,
	    queue_family_index,
	    queue_index,
	    false,  // external_fence_fd_enabled
	    false,  // external_semaphore_fd_enabled
	    false,  // timeline_semaphore_enabled
	    false,  // image_format_list_enabled
	    false,  // debug_utils_enabled
	    U_LOGGING_INFO);
	if (vk_ret != VK_SUCCESS) {
		U_LOG_E("Failed to initialize vk_bundle from app device: %d", vk_ret);
		free(c);
		return XRT_ERROR_VULKAN;
	}

#ifdef XRT_OS_WINDOWS
	/*
	 * #918 Phase 3 / ADR-037 §3 — THE VULKAN ANSWER, STATED.
	 *
	 * There is no output-device split for in-process Vulkan (ADR-037 open
	 * question 3: pending a decision, not just work — whole-app placement on the
	 * scanout adapter measured well enough that a VK bridge may never be worth
	 * building). So this path takes rung 2 unconditionally: everything on the
	 * render adapter, the OS carries the cross-adapter present.
	 *
	 * Rung 2 is a legitimate outcome; being SILENT about it is not, and until
	 * Phase 3 this path was. A hybrid box paying the cross-adapter present
	 * produced a Vulkan log byte-identical to a single-adapter box that pays
	 * nothing. It now emits the same one line the D3D paths do, with
	 * `reason=api_unsupported` — which is an honest NO, not a guess: the split
	 * is not implemented here, so the answer cannot be anything else.
	 *
	 * There is no half-engaged state to reach: nothing below this line consults
	 * a scanout adapter, allocates a bridge, or creates a second device.
	 */
	{
		uint64_t render_packed_luid = 0;
		if (c->vk.vkGetPhysicalDeviceProperties2 != NULL) {
			VkPhysicalDeviceIDProperties pdidp = {
			    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
			};
			VkPhysicalDeviceProperties2 pdp2 = {
			    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			    .pNext = &pdidp,
			};
			c->vk.vkGetPhysicalDeviceProperties2(c->vk.physical_device, &pdp2);
			if (pdidp.deviceLUIDValid == VK_TRUE) {
				memcpy(&render_packed_luid, pdidp.deviceLUID, sizeof(render_packed_luid));
			}
		}
		const uint32_t pw = (xdev != NULL && xdev->hmd != NULL) ? xdev->hmd->screens[0].w_pixels : 0;
		const uint32_t ph = (xdev != NULL && xdev->hmd != NULL) ? xdev->hmd->screens[0].h_pixels : 0;
		d3d_log_weave_placement(render_packed_luid, display_screen_left, display_screen_top, pw, ph,
		                        /* split_active */ false, COMP_SPLIT_REASON_API_UNSUPPORTED);
	}
#endif

#ifdef XRT_OS_MACOS
	// Import shared IOSurface if provided
	if (shared_texture_handle != NULL) {
		if (!import_shared_iosurface(c, shared_texture_handle)) {
			U_LOG_E("Failed to import shared IOSurface");
			free(c);
			return XRT_ERROR_VULKAN;
		}
	}
#endif

#ifdef XRT_OS_WINDOWS
	// Import shared D3D11 texture if provided
	if (shared_texture_handle != NULL) {
		if (!import_shared_d3d11_texture(c, shared_texture_handle)) {
			U_LOG_E("Failed to import shared D3D11 texture");
			free(c);
			return XRT_ERROR_VULKAN;
		}
	}

	// Handle window
	if (hwnd != NULL) {
		c->hwnd = hwnd;
		U_LOG_I("Using app-provided window handle: %p", hwnd);
	} else if (shared_texture_handle != NULL) {
		c->hwnd = NULL;
		U_LOG_I("Offscreen mode — no window (shared texture handle: %p)", shared_texture_handle);
	} else {
		uint32_t win_w = xdev->hmd->screens[0].w_pixels;
		uint32_t win_h = xdev->hmd->screens[0].h_pixels;
		if (win_w == 0 || win_h == 0) {
			win_w = 1920;
			win_h = 1080;
		}
		U_LOG_I("Creating self-owned window (%ux%u)", win_w, win_h);
		xrt_result_t xret = comp_d3d11_window_create(
		    win_w, win_h, display_screen_left, display_screen_top, &c->own_window);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to create self-owned window");
			free(c);
			return xret;
		}
		c->hwnd = comp_d3d11_window_get_hwnd(c->own_window);
		c->owns_window = true;
		U_LOG_I("Created self-owned window: %p", c->hwnd);
	}
#endif

#ifdef XRT_OS_MACOS
	// Handle window on macOS
	if (hwnd != NULL) {
		// hwnd is an NSView* from cocoa_window_binding
		xrt_result_t xret = comp_vk_native_window_macos_setup_external(
		    hwnd, transparent_background, &c->macos_window);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to set up external view for VK native");
			free(c);
			return xret;
		}
		c->owns_window = false;
		U_LOG_I("Using app-provided NSView for VK native compositor");
	} else if (shared_texture_handle != NULL) {
		c->macos_window = NULL;
		U_LOG_I("Offscreen mode — no window (shared texture)");
	} else {
		uint32_t win_w = xdev->hmd->screens[0].w_pixels;
		uint32_t win_h = xdev->hmd->screens[0].h_pixels;
		if (win_w == 0 || win_h == 0) {
			win_w = 1920;
			win_h = 1080;
		}
		// Open on the 3D panel at the plug-in-reported position (as the
		// Windows arm does), with the DXR_WINDOW_POS env override winning. #715.
		int32_t win_left = display_screen_left;
		int32_t win_top = display_screen_top;
		apply_window_pos_override(&win_left, &win_top);
		U_LOG_I("Creating self-owned macOS window (%ux%u at %d,%d)", win_w, win_h, win_left, win_top);
		xrt_result_t xret = comp_vk_native_window_macos_create(
		    win_w, win_h, win_left, win_top, transparent_background, &c->macos_window);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to create self-owned macOS window");
			free(c);
			return xret;
		}
		c->owns_window = true;
	}
	// Set hwnd to the CAMetalLayer for target creation
	if (c->macos_window != NULL) {
		hwnd = comp_vk_native_window_macos_get_layer(c->macos_window);
	}
#endif

#ifdef XRT_OS_LINUX_DESKTOP
	// hwnd is a struct comp_vk_native_xlib_handle* when the app supplied its
	// own X11 window via XR_DXR_xlib_window_binding (handle class, Phase 3);
	// NULL for hosted (runtime self-creates an XCB window, Phase 1).
#ifdef XRT_HAVE_WAYLAND
	if (window_is_wayland && hwnd != NULL) {
		// App-provided Wayland surface (XR_DXR_wayland_surface_binding, WS3b):
		// the target builds a VkWaylandSurfaceKHR from the pair directly — no
		// XCB window, no xdg-shell (the app owns the surface lifecycle).
		const struct comp_vk_native_wayland_handle *wl =
		    (const struct comp_vk_native_wayland_handle *)hwnd;
		c->wayland_handle = *wl;
		c->use_wayland = true;
		c->xcb_window = NULL;
		c->owns_window = false;
		U_LOG_I("Using app-provided Wayland surface (XR_DXR_wayland_surface_binding)");
#ifdef DXR_HAVE_WL_GEOM
		// Windowed weaving (#817): absolute window position via the
		// compositor's geometry service. NULL / no-data → display-scoped.
		c->wl_geom = comp_vk_native_wl_geom_create();
#endif
	} else
#endif
	    if (hwnd != NULL) {
		const struct comp_vk_native_xlib_handle *xlib =
		    (const struct comp_vk_native_xlib_handle *)hwnd;
		// Convert the Xlib display to its XCB connection (libX11-xcb) and
		// hand the target the same comp_vk_native_xcb_handle the hosted path
		// uses — the surface / metrics / resize plumbing downstream is shared.
		xrt_result_t xret = comp_vk_native_window_xcb_wrap_app_window(
		    xlib->display, xlib->window, &c->xcb_handle);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to derive XCB connection from app-provided Xlib display");
			free(c);
			return xret;
		}
		c->xcb_window = NULL;
		c->owns_window = false;
		hwnd = &c->xcb_handle;
		U_LOG_I("Using app-provided X11 window 0x%lx (XR_DXR_xlib_window_binding)",
		        xlib->window);
	} else if (shared_texture_handle != NULL) {
		c->xcb_window = NULL;
		U_LOG_I("Offscreen mode — no window (shared texture)");
		hwnd = NULL;
	} else {
		uint32_t win_w = xdev->hmd->screens[0].w_pixels;
		uint32_t win_h = xdev->hmd->screens[0].h_pixels;
		if (win_w == 0 || win_h == 0) {
			win_w = 1920;
			win_h = 1080;
		}

#ifdef DXR_HAVE_DIRECT_SCANOUT
		// Opt-in direct scanout (ST-5539): fullscreen-only, reclaims the ~44%
		// Xorg+compositor presentation cost. Acquire the 3D-panel connector and
		// scan out to it directly. Any failure (exts absent, no leasable
		// connector) leaves direct_window NULL → fall through to XCB below, so
		// the default path is untouched.
		const char *ds_env = getenv("DXR_LINUX_DIRECT_SCANOUT");
		if (ds_env != NULL && ds_env[0] == '1') {
			xrt_result_t dret = comp_vk_native_window_direct_create(
			    &c->vk, display_screen_left, display_screen_top, &c->direct_window);
			if (dret == XRT_SUCCESS) {
				c->owns_window = true;
				// hwnd stays NULL; the target is built from the backend's
				// display-plane surface after settings init.
				U_LOG_W("Direct-scanout present path active (bypassing Xorg/compositor)");
			} else {
				U_LOG_W("DXR_LINUX_DIRECT_SCANOUT set but direct scanout "
				        "unavailable (%d) — falling back to XCB present",
				        (int)dret);
			}
		}
		if (c->direct_window == NULL)
#endif
		{
			// Open on the 3D panel at the plug-in-reported position (as the
			// Windows arm does), with the DXR_WINDOW_POS env override winning. #715.
			int32_t win_left = display_screen_left;
			int32_t win_top = display_screen_top;
			apply_window_pos_override(&win_left, &win_top);
			U_LOG_I("Creating self-owned XCB window (%ux%u at %d,%d)", win_w, win_h, win_left,
			        win_top);
			xrt_result_t xret = comp_vk_native_window_xcb_create(
			    win_w, win_h, win_left, win_top, transparent_background, &c->xcb_window);
			if (xret != XRT_SUCCESS) {
				U_LOG_E("Failed to create self-owned XCB window");
				free(c);
				return xret;
			}
			c->owns_window = true;
			// Hand the connection + window id to the target via the type-erased
			// hwnd. c->xcb_handle lives in the compositor, so the connection the
			// surface borrows stays valid for the target's lifetime.
			comp_vk_native_window_xcb_get_handle(c->xcb_window, &c->xcb_handle);
			hwnd = &c->xcb_handle;
		}
	}
#endif

	// Initialize settings
	memset(&c->settings, 0, sizeof(c->settings));
	c->settings.preferred.width = xdev->hmd->screens[0].w_pixels;
	c->settings.preferred.height = xdev->hmd->screens[0].h_pixels;
	if (c->settings.preferred.width == 0 || c->settings.preferred.height == 0) {
		c->settings.preferred.width = 1920;
		c->settings.preferred.height = 1080;
	}
	c->settings.nominal_frame_interval_ns = xdev->hmd->screens[0].nominal_frame_interval_ns;
	if (c->settings.nominal_frame_interval_ns == 0) {
		c->settings.nominal_frame_interval_ns = (1000 * 1000 * 1000) / 60;
	}

#ifdef XRT_OS_WINDOWS
	if (c->hwnd != NULL) {
		RECT rect;
		if (GetClientRect((HWND)c->hwnd, &rect)) {
			c->settings.preferred.width = (uint32_t)(rect.right - rect.left);
			c->settings.preferred.height = (uint32_t)(rect.bottom - rect.top);
		}
	}
#endif

#ifdef XRT_OS_MACOS
	if (c->macos_window != NULL) {
		uint32_t mac_w = 0, mac_h = 0;
		comp_vk_native_window_macos_get_dimensions(c->macos_window, &mac_w, &mac_h);
		if (mac_w > 0 && mac_h > 0) {
			c->settings.preferred.width = mac_w;
			c->settings.preferred.height = mac_h;
		}
	}
	// Shared IOSurface dimensions take priority (import_shared_iosurface set them,
	// but settings init above may have overwritten with screen logical dimensions)
	if (c->has_shared_texture && shared_texture_handle != NULL) {
		IOSurfaceRef surface = (IOSurfaceRef)shared_texture_handle;
		c->settings.preferred.width = (uint32_t)IOSurfaceGetWidth(surface);
		c->settings.preferred.height = (uint32_t)IOSurfaceGetHeight(surface);
	}
#endif

#ifdef XRT_OS_LINUX_DESKTOP
#ifdef DXR_HAVE_DIRECT_SCANOUT
	if (c->direct_window != NULL) {
		// Direct scanout: dimensions are the connector's fixed native mode.
		uint32_t dw = 0, dh = 0;
		comp_vk_native_window_direct_get_dimensions(c->direct_window, &dw, &dh);
		if (dw > 0 && dh > 0) {
			c->settings.preferred.width = dw;
			c->settings.preferred.height = dh;
		}
	} else
#endif
	    if (c->xcb_window != NULL) {
		uint32_t xw = 0, xh = 0;
		comp_vk_native_window_xcb_get_dimensions(c->xcb_window, &xw, &xh);
		if (xw > 0 && xh > 0) {
			c->settings.preferred.width = xw;
			c->settings.preferred.height = xh;
		}
	} else if (!c->owns_window && c->xcb_handle.connection != NULL) {
		// App-provided window (XR_DXR_xlib_window_binding) — no helper
		// tracking ConfigureNotify, so query the live size directly.
		uint32_t xw = 0, xh = 0;
		if (comp_vk_native_window_xcb_query_geometry(&c->xcb_handle, &xw, &xh) &&
		    xw > 0 && xh > 0) {
			c->settings.preferred.width = xw;
			c->settings.preferred.height = xh;
		}
	}
#endif

	// Default refresh rate, replaced with the monitor's CURRENT mode where we
	// can read it. A hardcoded 60 hands the display processor a frame period
	// 2–4× too long on a high-refresh panel (2.75× on 165 Hz; a 120/144/240 Hz
	// Odyssey held each #868 interlace pattern for several refreshes) — Windows
	// via EnumDisplaySettings, desktop Linux via RandR.
	c->display_refresh_rate = 60.0f;
#ifdef XRT_OS_WINDOWS
	{
		const float hz = comp_display_refresh_hz_win(c->hwnd);
		if (hz > 0.0f) {
			c->display_refresh_rate = hz;
		}
		U_LOG_W("Display refresh rate: %.2f Hz (frame period %.2f ms)", c->display_refresh_rate,
		        1000.0 / c->display_refresh_rate);
		// The target does not exist yet at this point in create — the period
		// is seeded into it right after target creation below.
	}
#endif
#ifdef XRT_OS_LINUX_DESKTOP
	{
		// Works for both the self-owned hosted window and an app-provided
		// xlib/xcb window; either populates an xcb_handle. (Direct-scanout is
		// its connector's fixed native mode — a follow-up, not this windowed
		// path.)
		struct comp_vk_native_xcb_handle refresh_handle = c->xcb_handle;
		if (refresh_handle.connection == NULL && c->xcb_window != NULL) {
			comp_vk_native_window_xcb_get_handle(c->xcb_window, &refresh_handle);
		}
		if (refresh_handle.connection != NULL) {
			float hz = 0.0f;
			if (comp_vk_native_window_xcb_query_refresh_hz(&refresh_handle, &hz) && hz > 0.0f) {
				c->display_refresh_rate = hz;
			}
			U_LOG_W("Display refresh rate: %.2f Hz (frame period %.2f ms)", c->display_refresh_rate,
			        1000.0 / c->display_refresh_rate);
			// Seeded into the target right after target creation below.
		}
	}
#endif


	// Create display processor via factory FIRST — the SR weaver creates
	// its own VkSwapchain on the HWND, so we must not also create one.
	if (dp_factory_vk != NULL) {
		xrt_dp_factory_vk_fn_t factory = (xrt_dp_factory_vk_fn_t)dp_factory_vk;

		// Create command pool for display processor (SR weaver needs it)
		VkCommandPoolCreateInfo pool_ci = {
		    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		    .queueFamilyIndex = c->queue_family_index,
		};
		VkResult pool_ret = c->vk.vkCreateCommandPool(
		    c->vk.device, &pool_ci, NULL, &c->cmd_pool);
		if (pool_ret != VK_SUCCESS) {
			U_LOG_E("Failed to create command pool for display processor: %d", pool_ret);
			vk_compositor_destroy(&c->base.base);
			return XRT_ERROR_VULKAN;
		}

		// Window handed to the DP → the SR weaver. It is NOT for swapchain
		// creation (the Linux/Vulkan weaver makes none — it renders into our
		// framebuffer); it is the weaver's window-GEOMETRY input for
		// getDrawRegions() — the per-view interlacing draw-regions AND the
		// window-anchored eye-tracked steering. Passing NULL selects the srSDK's
		// windowless/display-scoped path (constructedWithoutWindow=true), which
		// weaves for the DEFAULT viewpoint with no window geometry (#778: view
		// swap on Linux). The working srSDK vulkan_weaving_linux example passes
		// the real X11 window here, which is why it tracks. So on desktop-Linux
		// pass the XCB window XID (populated for BOTH the self-created/hosted and
		// the app-provided/handle window via c->xcb_handle above). macOS keeps
		// NULL — its weave is the IOSurface shared-surface path, no window.
		void *dp_window_handle = NULL;
#ifdef XRT_OS_WINDOWS
		dp_window_handle = c->hwnd;
#elif defined(XRT_OS_LINUX_DESKTOP)
		dp_window_handle = (void *)(uintptr_t)c->xcb_handle.window;
		U_LOG_W("VK DP factory: passing X11 window XID 0x%lx to the weaver (0 = windowless/display-scoped)",
		        (unsigned long)c->xcb_handle.window);
#endif
		/*
		 * #868: resolve the runtime-owned queue BEFORE the display processor
		 * is created — the DP captures whichever queue it is shown here, once,
		 * and never re-reads it.
		 */
		/*
		 * Belt and braces: refuse a "runtime-owned" queue that is actually the
		 * APP's. Defaulting these to 0 instead of -1 once made them name
		 * family 0 / queue 0 under vulkan_enable1 — the app's own queue — and
		 * the repaint lost the device submitting to it. An identity check costs
		 * nothing and makes that unrepeatable.
		 */
		if (c->repaint_queue_family == (int32_t)queue_family_index &&
		    c->repaint_queue_index == (int32_t)queue_index) {
			U_LOG_W("#868: refusing repaint queue (family %d index %d) — that is the APP's "
			        "own queue, not a runtime-owned one",
			        c->repaint_queue_family, c->repaint_queue_index);
			c->repaint_queue_family = -1;
			c->repaint_queue_index = -1;
		}
		if (c->repaint_queue_family >= 0 && c->repaint_queue_index >= 0) {
			c->vk.vkGetDeviceQueue(c->vk.device, (uint32_t)c->repaint_queue_family,
			                       (uint32_t)c->repaint_queue_index, &c->repaint_queue);
		}

		/*
		 * #868: hand the display processor the RUNTIME-OWNED queue, not the
		 * app's.
		 *
		 * The vendor DP reads vk->main_queue->queue ONCE, here at creation, and
		 * the SR weaver captures it internally for its own submits — the
		 * correction-texture upload, which vkQueueSubmit()s and waits from
		 * inside weave(). weave() is called from the repaint thread too, so
		 * with the app's queue captured, the SDK submits to the APP's queue off
		 * the app's thread. A VkQueue is externally synchronised, so that is
		 * undefined behaviour, and it is what validation reports as
		 * "UNASSIGNED-Threading-MultipleThreads-Write". Confirmed by handle:
		 * the queue named in the error is the app's, never the repaint one.
		 *
		 * The runtime's own submits already moved to the repaint queue, but
		 * that never covered the SDK's internal ones. Since the capture happens
		 * exactly once and reads this pointer, pointing it at the runtime queue
		 * across the factory call moves every weaver-internal submit onto a
		 * queue only the runtime touches, serialised by c->mutex.
		 *
		 * Safe: both queues live in the SAME family, so images need no
		 * queue-family ownership transfer, and the weaver vkQueueWaitIdle()s
		 * its own queue before returning, which CPU-orders those uploads ahead
		 * of whichever queue we later submit the recorded weave on.
		 *
		 * The app frame keeps submitting on the APP's queue deliberately — that
		 * is what orders the compositor's work after the app's own rendering
		 * into the swapchain images it just released. Moving the app path to
		 * the runtime queue would silently drop that ordering.
		 *
		 * Restored immediately; the repaint thread does not exist yet.
		 */
		VkQueue saved_main_queue = c->vk.main_queue->queue;
		if (c->repaint_queue != VK_NULL_HANDLE) {
			c->vk.main_queue->queue = c->repaint_queue;
			U_LOG_W("#868: creating the display processor against the runtime-owned queue %p "
			        "(app queue %p stays the app frame's)",
			        (void *)c->repaint_queue, (void *)saved_main_queue);
		}
		xrt_result_t dp_ret = factory(&c->vk, (void *)(uintptr_t)c->cmd_pool,
		                               dp_window_handle,
		                               (int32_t)VK_FORMAT_B8G8R8A8_UNORM,
		                               &c->display_processor);
		c->vk.main_queue->queue = saved_main_queue;
		if (dp_ret != XRT_SUCCESS) {
			U_LOG_W("VK display processor factory failed (error %d), continuing without",
			        (int)dp_ret);
			c->display_processor = NULL;
		} else {
			U_LOG_W("VK display processor created via factory");
		}
	} else {
		U_LOG_W("No VK display processor factory provided");
	}

	// Forward transparent_background to the display processor (#573 —
	// chroma-key-free). Safe no-op if the DP lacks the slot (sim_display) or
	// display_processor is NULL.
	//
	// client_presents=false — DELIBERATELY; #904's true was reverted after a
	// hardware eyeball. The de-occlusion band (pixels where SOME but not all
	// views are transparent — the parallax fringe around 3D content) cannot
	// come from DWM live blending: the SR weaver destroys per-pixel alpha and
	// the alpha-gate reconstructs only the binary all-views-transparent mask.
	// The band is either the DP's compose-under-bg (~1-frame bake — the
	// product spec: live desktop in the holes, bake only in the band) or it
	// is BLACK. #904 disabled the compose calling it wasted work; the dwm
	// saving partly bought black de-occlusions. WGC cost is attacked via
	// capture throttling instead. client_presents=true remains correct for
	// true client-side presents (#551 IPC) and bandless content.
	if (c->display_processor != NULL) {
		xrt_display_processor_vk_set_transparent_background(
		    (struct xrt_display_processor_vk *)c->display_processor, transparent_background,
		    false);

		// #613 / #68 — tell the DP whether the app self-presents only the canvas
		// (shared-texture apps) vs the full target (handle apps). Gates the
		// compose-under-bg desktop-UV remap skip for `_texture` zones frames so
		// the captured desktop isn't magnified. Set once here; safe no-op if the
		// DP lacks the slot (sim_display / older plug-in).
		xrt_display_processor_vk_set_shared_texture_present(
		    (struct xrt_display_processor_vk *)c->display_processor, c->has_shared_texture);
	}

	// Create output target (VkSwapchainKHR) for presentation.
	// The compositor owns the swapchain — the weaver (display processor)
	// records interlacing commands into a caller-provided command buffer +
	// framebuffer via setCommandBuffer / setOutputFrameBuffer. It does NOT
	// create its own swapchain. The HWND passed to CreateVulkanWeaver is
	// used only for monitor detection and draw-region calculation.
	if (hwnd != NULL
#ifdef XRT_OS_WINDOWS
	    || c->owns_window
#endif
#ifdef XRT_OS_MACOS
	    || c->owns_window
#endif
#ifdef XRT_OS_LINUX_DESKTOP
	    || c->owns_window
#endif
	) {
		xrt_result_t xret;
#ifdef DXR_HAVE_DIRECT_SCANOUT
		if (c->direct_window != NULL) {
			// Direct scanout: the backend already built the display-plane
			// surface — hand it straight to the target (no hwnd).
			xret = comp_vk_native_target_create_from_surface(
			    c, comp_vk_native_window_direct_get_surface(c->direct_window),
			    c->settings.preferred.width, c->settings.preferred.height, &c->target);
		} else
#endif
		{
			void *target_hwnd = hwnd;
			bool target_is_wayland = false;
#ifdef XRT_OS_WINDOWS
			if (target_hwnd == NULL) target_hwnd = c->hwnd;
#endif
#ifdef XRT_OS_LINUX_DESKTOP
			if (target_hwnd == NULL && c->xcb_window != NULL) target_hwnd = &c->xcb_handle;
#endif
#ifdef XRT_HAVE_WAYLAND
			if (c->use_wayland) {
				target_hwnd = &c->wayland_handle;
				target_is_wayland = true;
			}
#endif
			xret = comp_vk_native_target_create(c, target_hwnd, target_is_wayland,
			                                    c->settings.preferred.width,
			                                    c->settings.preferred.height,
			                                    transparent_background, &c->target);
		}
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to create VK target");
			vk_compositor_destroy(&c->base.base);
			return xret;
		}
		// Seed the display period measured above — the two detection blocks
		// run before the target exists, so seeding there was dead code and
		// left period_hint_ns at 0 (no #867 lookahead, and the #912 pacing
		// governor gated itself off entirely: period 0 reads as "unknown").
		if (c->display_refresh_rate > 0.0f) {
			comp_vk_native_target_set_display_period(
			    c->target, (uint64_t)(U_TIME_1S_IN_NS / c->display_refresh_rate));
		}
	} else {
		c->target = NULL;
		U_LOG_I("No VK target — offscreen shared texture mode");
	}

	/*
	 * #868: the repaint loop, ON by default (DXR_WEAVE_REPAINT=0 disables),
	 * matching D3D11 and D3D12. Only meaningful with a window target.
 *
 * MUST come after target creation: the display-processor factory runs
 * before it, so at the refresh-rate probe further up c->target is still
 * NULL and the guard below would silently skip the start.
	 */
	{
		/*
		 * Why the repaint needs MORE than the env var on VK.
		 *
		 * vk->main_queue->queue is the APP's queue. A VkQueue is "externally
		 * synchronised", which means the APPLICATION must serialise access —
		 * and the app submits its own scene rendering on its own thread,
		 * outside any OpenXR call and outside any lock the runtime controls.
		 * c->mutex serialises the compositor's own two callers and cannot
		 * cover the app's submits, so a bare repaint thread racing them is
		 * unsafe by construction. Validation confirms it:
		 *
		 *   vkQueueSubmit(): THREADING ERROR : object of type VkQueue is
		 *   simultaneously used in current thread A and thread B
		 *
		 * and the result is VK_ERROR_DEVICE_LOST (-4), after which the app
		 * fails every frame. Measured on cube_zones_vk_win: zero validation
		 * errors with the loop off, five distinct classes with it on.
		 *
		 * D3D11 had the same race on the app's immediate context and WAS
		 * fixable, because ID3D10Multithread::Enter/Leave takes a lock the
		 * DRIVER enforces for every caller including the app. Vulkan exposes
		 * no such lock — so the runtime supplies safety one of two ways
		 * (#902, docs/roadmap/vk-late-weave-queue-serialization.md):
		 *
		 *   tier 1 — a queue the runtime owns exclusively (reserved at
		 *            xrCreateVulkanDeviceKHR when the graphics family has a
		 *            spare queue; NVIDIA);
		 *   tier 2 — the SAME queue as the app, made safe by the injected
		 *            VK_LAYER_DXR_queue_lock, which is the D3D11 driver lock
		 *            recreated at the loader level: every vkQueue* call from
		 *            every in-process caller (app engine, repaint thread,
		 *            vendor DP internals) takes a per-queue mutex INSIDE the
		 *            call (single-graphics-queue GPUs: Intel iGPUs, AMD).
		 *
		 * The loop is gated on actually having one of the two, not merely on
		 * the env var.
		 */
		const char *e = getenv("DXR_WEAVE_REPAINT");
		c->repaint.enabled = (e != NULL && e[0] == '0') ? 0 : 1;

		/*
		 * #902: tier selection. DXR_VK_QUEUE_MODE:
		 *   auto (default) — dedicated runtime queue where the driver gave
		 *                    one (tier 1); else shared-queue serialized by
		 *                    VK_LAYER_DXR_queue_lock (tier 2); else repaint
		 *                    off, pacing only (tier 3).
		 *   queue — tier 1 only (no layer use).
		 *   layer — force tier 2 even when a runtime queue exists, so the
		 *           layer path is testable on multi-queue GPUs.
		 *   off   — no layer injection (oxr_vulkan.c) and no repaint.
		 */
		const char *qm = getenv("DXR_VK_QUEUE_MODE");
		bool mode_layer_ok = (qm == NULL) || (strcmp(qm, "auto") == 0) || (strcmp(qm, "layer") == 0);
		if (qm != NULL && strcmp(qm, "off") == 0) {
			c->repaint.enabled = 0;
		}
		if (qm != NULL && strcmp(qm, "layer") == 0 && c->repaint_queue != VK_NULL_HANDLE) {
			U_LOG_W("#902: DXR_VK_QUEUE_MODE=layer — ignoring the runtime-owned queue (test "
			        "override, forcing the shared-queue tier)");
			c->repaint_queue = VK_NULL_HANDLE;
		}

		/*
		 * #902 marker handshake: is the queue-lock layer live in THIS
		 * device's chain? The repaint may only share the app's queue when
		 * the layer actually serializes it — resolve the layer's marker
		 * entry point rather than assume the injection worked.
		 */
		bool layer_live = c->vk.vkGetDeviceProcAddr != NULL &&
		                  c->vk.vkGetDeviceProcAddr(c->vk.device, "vkGetQueueLockMarkerDXR") != NULL;
		c->repaint.shared_queue = (c->repaint_queue == VK_NULL_HANDLE && layer_live && mode_layer_ok &&
		                           c->repaint.enabled == 1)
		                              ? 1
		                              : 0;

		if (c->repaint_queue == VK_NULL_HANDLE && c->repaint.shared_queue == 0) {
			if (c->repaint.enabled == 1) {
				U_LOG_W("#868: no runtime-owned VkQueue (vulkan_enable1, or the graphics "
				        "family was saturated) and no queue-lock layer (#902 marker %s) — "
				        "repaint disabled. Sharing the app's queue bare is undefined "
				        "behaviour and loses the device.",
				        layer_live ? "resolved but mode excludes it" : "absent");
			}
			c->repaint.enabled = 0;
		} else {
			/*
			 * A command pool is externally synchronised too, so the replay
			 * cannot record from the renderer's pool while the app frame
			 * uses it concurrently. Give the repaint its own — on the
			 * runtime queue's family (tier 1) or the app's family (tier 2;
			 * same family by construction, it IS the graphics family).
			 */
			uint32_t rp_family = c->repaint_queue != VK_NULL_HANDLE
			                         ? (uint32_t)c->repaint_queue_family
			                         : c->queue_family_index;
			VkCommandPoolCreateInfo rp_ci = {
			    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			    .queueFamilyIndex = rp_family,
			};
			if (c->vk.vkCreateCommandPool(c->vk.device, &rp_ci, NULL, &c->repaint_cmd_pool) !=
			    VK_SUCCESS) {
				// Also drop enabled: the old code left it set here, and the
				// start gate below would spin up a repaint thread with no
				// queue to submit on.
				U_LOG_W("#868: failed to create the repaint command pool — repaint disabled");
				c->repaint_queue = VK_NULL_HANDLE;
				c->repaint.shared_queue = 0;
				c->repaint.enabled = 0;
			} else if (c->repaint_queue != VK_NULL_HANDLE) {
				U_LOG_W("#868: repaint will submit on the runtime-owned queue "
				        "(family %d index %d) with its own fence and command pool",
				        c->repaint_queue_family, c->repaint_queue_index);
				// Log both handles so a validation "VkQueue is simultaneously
				// used in thread A and thread B" can be attributed to a
				// specific queue instead of guessed at.
				U_LOG_W("#868: queue handles — app=%p repaint=%p",
				        (void *)c->vk.main_queue->queue, (void *)c->repaint_queue);
			} else {
				U_LOG_W("#902: repaint will submit on the APP's queue %p, serialized "
				        "per-call by VK_LAYER_DXR_queue_lock (shared-queue tier — single "
				        "graphics-queue GPU)",
				        (void *)c->vk.main_queue->queue);
			}
		}
		const char *fe = getenv("DXR_WEAVE_REPAINT_FORCE");
		c->repaint.force = (fe != NULL && fe[0] == '1') ? 1 : 0;
		if (c->repaint.force == 1) {
			U_LOG_W("#868: DXR_WEAVE_REPAINT_FORCE=1 — repainting every refresh regardless "
			        "of app rate. Correctness probe; it WILL cost frame rate.");
		}
		if (c->repaint.enabled == 1 && c->target != NULL) {
			// Seed the quiet-gate key so the first force-probe counter row
			// doesn't log a garbage quiet_ns (now − 0) before the first app
			// frame publishes (#902 Windows validation, cosmetic).
			c->repaint.last_app_frame_ns = os_monotonic_get_ns();
			int sret = os_thread_helper_start(&c->repaint_thread, vk_repaint_thread, c);
			U_LOG_W("#868: repaint loop start ret=%d (target=%p)", sret, (void *)c->target);
		} else {
			// Silence here is what hid the loop never starting once already.
			U_LOG_W("#868: repaint loop NOT started (enabled=%d target=%p)", c->repaint.enabled,
			        (void *)c->target);
		}
	}

	// Guard: target/swapchain creation above can trigger GPU-driver Vulkan
	// shader-compiler heap churn that, on some configs (Windows + Leia SR),
	// reuses the display-processor's heap block and corrupts its vtable. A
	// call through a garbage vtable slot (e.g. get_display_pixel_info just
	// below) is a hard crash. Validate here and degrade to passthrough
	// (no weaving) instead of crashing. Do NOT call the DP's destroy — that
	// pointer may itself be garbage; leak the corrupt object on this path.
	if (c->display_processor != NULL && !dp_vtable_looks_sane(c->display_processor)) {
		U_LOG_E("VK display processor vtable corrupt after target creation — "
		        "disabling DP (output will not be woven). Likely a plug-in built "
		        "against a different runtime ABI major (ADR-020), or a driver "
		        "heap-reuse collision.");
		c->display_processor = NULL;
	}

	// Determine view dimensions
	uint32_t view_width = c->settings.preferred.width / 2;
	uint32_t view_height = c->settings.preferred.height;

	// If display processor is available, query display info for view dimensions
	if (c->display_processor != NULL) {
		uint32_t disp_px_w = 0, disp_px_h = 0;
		int32_t disp_left = 0, disp_top = 0;
		if (xrt_display_processor_get_display_pixel_info(
		        c->display_processor, &disp_px_w, &disp_px_h,
		        &disp_left, &disp_top) &&
		    disp_px_w > 0 && disp_px_h > 0) {
			uint32_t base_vw = disp_px_w / 2;
			uint32_t base_vh = disp_px_h;

			float ratio = fminf(
			    (float)c->settings.preferred.width / (float)disp_px_w,
			    (float)c->settings.preferred.height / (float)disp_px_h);
			if (ratio > 1.0f) ratio = 1.0f;

			view_width = (uint32_t)((float)base_vw * ratio);
			view_height = (uint32_t)((float)base_vh * ratio);
		}
	}

	// Compute atlas dimensions from active rendering mode
	uint32_t tile_cols = 2, tile_rows = 1;
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			const struct xrt_rendering_mode *mode = &c->xdev->rendering_modes[idx];
			if (mode->tile_columns > 0) {
				tile_cols = mode->tile_columns;
				tile_rows = mode->tile_rows;
			}
		}
	}
	uint32_t atlas_width = tile_cols * view_width;
	uint32_t atlas_height = tile_rows * view_height;

	// Create renderer with active mode atlas
	xrt_result_t xret = comp_vk_native_renderer_create(c, view_width, view_height,
	                                                     atlas_width, atlas_height,
	                                                     c->app_timeline_semaphores, &c->renderer);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to create VK renderer");
		vk_compositor_destroy(&c->base.base);
		return xret;
	}

	// Clear the atlas transparent (alpha=0) instead of opaque black when a
	// transparent background was requested, so app alpha<1 regions survive
	// to the present (issue #392). Mirrors the DP weave-clear plumbing above.
	comp_vk_native_renderer_set_transparent(c->renderer, transparent_background);
	c->transparent_background = transparent_background;

	// Set tile layout from active rendering mode
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count && c->xdev->rendering_modes[idx].tile_columns > 0) {
			comp_vk_native_renderer_set_tile_layout(
			    c->renderer,
			    c->xdev->rendering_modes[idx].tile_columns,
			    c->xdev->rendering_modes[idx].tile_rows);
		}
	}

	// Create HUD overlay for runtime-owned windows
#if defined(XRT_OS_WINDOWS) || defined(XRT_OS_MACOS) || defined(XRT_OS_LINUX_DESKTOP)
	if (c->owns_window) {
		u_hud_create(&c->hud, c->settings.preferred.width);
	}
#endif

	// Initialize layer accumulator
	memset(&c->layer_accum, 0, sizeof(c->layer_accum));

	// #439 Phase 3 — masked 2D-over-3D composite pipelines. The target and the
	// runtime-owned scratch are both B8G8R8A8_UNORM (the windowed target prefers
	// it, the shared IOSurface image is it, and the weave snapshot is a raw copy
	// of the target), so the render-pass formats are fixed. Created eagerly so
	// the zone-mask API (which can be called before the first frame) has its R8
	// raster render pass available.
	c->local2d_initialized = vk_local2d_composite_init(&c->local2d, &c->vk, VK_FORMAT_B8G8R8A8_UNORM,
	                                                   VK_FORMAT_B8G8R8A8_UNORM);
	if (!c->local2d_initialized) {
		U_LOG_W("VK Local2D composite init failed — 2D-over-3D masking disabled this session");
	}

	// Populate supported swapchain formats (Vulkan formats)
	uint32_t format_count = 0;
#ifndef XRT_OS_ANDROID
	// BGRA is the desktop swapchain-native order (DXGI back-buffers and
	// IOSurfaces are BGRA8), so list it first there. On Android it must NOT be
	// advertised: app swapchain images are AHardwareBuffer-backed, and Adreno's
	// gralloc cannot allocate B8G8R8A8 (gralloc rejects format 52 →
	// "isSupported(...,52,...) failed", VK_FORMAT_B8G8R8A8_UNORM probes false).
	// Advertising it makes an app that prefers BGRA (e.g. cube_handle_vk_android)
	// fail xrCreateSwapchain → no reference space → black screen. RGBA8 is the
	// Adreno-allocatable order and is advertised first below. (#496)
	c->base.base.info.formats[format_count++] = VK_FORMAT_B8G8R8A8_UNORM;
	c->base.base.info.formats[format_count++] = VK_FORMAT_B8G8R8A8_SRGB;
#endif
	c->base.base.info.formats[format_count++] = VK_FORMAT_R8G8B8A8_UNORM;
	c->base.base.info.formats[format_count++] = VK_FORMAT_R8G8B8A8_SRGB;
	c->base.base.info.formats[format_count++] = VK_FORMAT_R16G16B16A16_SFLOAT;
	c->base.base.info.formats[format_count++] = VK_FORMAT_R16G16B16A16_UNORM;
	c->base.base.info.formats[format_count++] = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	c->base.base.info.formats[format_count++] = VK_FORMAT_D16_UNORM;
	c->base.base.info.formats[format_count++] = VK_FORMAT_D32_SFLOAT;
	c->base.base.info.formats[format_count++] = VK_FORMAT_D24_UNORM_S8_UINT;
	c->base.base.info.format_count = format_count;

	// Native compositor is always visible and focused
	c->base.base.info.initial_visible = true;
	c->base.base.info.initial_focused = true;

	// Set up compositor interface
	c->base.base.get_swapchain_create_properties = vk_compositor_get_swapchain_create_properties;
	c->base.base.create_swapchain = vk_compositor_create_swapchain;
	c->base.base.import_swapchain = vk_compositor_import_swapchain;
	c->base.base.import_fence = vk_compositor_import_fence;
	c->base.base.create_semaphore = vk_compositor_create_semaphore;
	c->base.base.begin_session = vk_compositor_begin_session;
	c->base.base.end_session = vk_compositor_end_session;
	c->base.base.wait_frame = vk_compositor_wait_frame;
	c->base.base.predict_frame = vk_compositor_predict_frame;
	c->base.base.mark_frame = vk_compositor_mark_frame;
	c->base.base.begin_frame = vk_compositor_begin_frame;
	c->base.base.discard_frame = vk_compositor_discard_frame;
	c->base.base.layer_begin = vk_compositor_layer_begin;
	c->base.base.layer_projection = vk_compositor_layer_projection;
	c->base.base.layer_projection_depth = vk_compositor_layer_projection_depth;
	c->base.base.layer_quad = vk_compositor_layer_quad;
	c->base.base.layer_cube = vk_compositor_layer_cube;
	c->base.base.layer_cylinder = vk_compositor_layer_cylinder;
	c->base.base.layer_equirect1 = vk_compositor_layer_equirect1;
	c->base.base.layer_equirect2 = vk_compositor_layer_equirect2;
	c->base.base.layer_passthrough = vk_compositor_layer_passthrough;
	c->base.base.layer_window_space = vk_compositor_layer_window_space;
	c->base.base.layer_local_2d = vk_compositor_layer_local_2d;
	c->base.base.layer_zone_3d = vk_compositor_layer_zone_3d;
	c->base.base.layer_commit = vk_compositor_layer_commit;
	c->base.base.layer_commit_with_semaphore = vk_compositor_layer_commit_with_semaphore;
	c->base.base.destroy = vk_compositor_destroy;

	// Install MCP capture_frame hook + arm the trigger-file path (#210).
	mcp_capture_init(&c->mcp_capture);
	mcp_capture_install(&c->mcp_capture);

	*out_xc = &c->base;

	U_LOG_I("VK native compositor created successfully (%ux%u)",
	        c->settings.preferred.width, c->settings.preferred.height);

	return XRT_SUCCESS;
}

bool
comp_vk_native_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                                      struct xrt_eye_positions *out_eye_pos)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

	if (c->display_processor != NULL) {
		if (xrt_display_processor_get_predicted_eye_positions(c->display_processor, out_eye_pos) &&
		    out_eye_pos->valid) {
			return true;
		}
	}

	return false;
}

bool
comp_vk_native_compositor_get_display_dimensions(struct xrt_compositor *xc,
                                                  float *out_width_m,
                                                  float *out_height_m)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

	if (c->display_processor != NULL) {
		if (xrt_display_processor_get_display_dimensions(
		        c->display_processor, out_width_m, out_height_m)) {
			return true;
		}
	}

	// Fallback to system compositor info (sim_display DP doesn't implement get_display_dimensions)
	if (c->sys_info_set && c->sys_info.display_width_m > 0.0f && c->sys_info.display_height_m > 0.0f) {
		*out_width_m = c->sys_info.display_width_m;
		*out_height_m = c->sys_info.display_height_m;
		return true;
	}

	*out_width_m = 0.3f;
	*out_height_m = 0.2f;
	return false;
}

bool
comp_vk_native_compositor_get_window_metrics(struct xrt_compositor *xc,
                                              struct xrt_window_metrics *out_metrics)
{
	if (xc == NULL || out_metrics == NULL) {
		if (out_metrics != NULL) out_metrics->valid = false;
		return false;
	}

	struct comp_vk_native_compositor *c = vk_comp(xc);
	memset(out_metrics, 0, sizeof(*out_metrics));

#ifdef XRT_OS_WINDOWS
	if (c->display_processor == NULL || c->hwnd == NULL) {
		return false;
	}

	uint32_t disp_px_w = 0, disp_px_h = 0;
	int32_t disp_left = 0, disp_top = 0;
	if (!xrt_display_processor_get_display_pixel_info(
	        c->display_processor, &disp_px_w, &disp_px_h,
	        &disp_left, &disp_top)) {
		return false;
	}
	if (disp_px_w == 0 || disp_px_h == 0) return false;

	float disp_w_m = 0.0f, disp_h_m = 0.0f;
	if (!xrt_display_processor_get_display_dimensions(
	        c->display_processor, &disp_w_m, &disp_h_m)) {
		return false;
	}

	RECT rect;
	if (!GetClientRect((HWND)c->hwnd, &rect)) return false;
	uint32_t win_px_w = (uint32_t)(rect.right - rect.left);
	uint32_t win_px_h = (uint32_t)(rect.bottom - rect.top);
	if (win_px_w == 0 || win_px_h == 0) return false;

	POINT client_origin = {0, 0};
	ClientToScreen((HWND)c->hwnd, &client_origin);

	float pixel_size_x = disp_w_m / (float)disp_px_w;
	float pixel_size_y = disp_h_m / (float)disp_px_h;

	out_metrics->display_width_m = disp_w_m;
	out_metrics->display_height_m = disp_h_m;
	out_metrics->display_pixel_width = disp_px_w;
	out_metrics->display_pixel_height = disp_px_h;
	out_metrics->display_screen_left = disp_left;
	out_metrics->display_screen_top = disp_top;

	out_metrics->window_pixel_width = win_px_w;
	out_metrics->window_pixel_height = win_px_h;
	out_metrics->window_screen_left = (int32_t)client_origin.x;
	out_metrics->window_screen_top = (int32_t)client_origin.y;

	out_metrics->window_width_m = (float)win_px_w * pixel_size_x;
	out_metrics->window_height_m = (float)win_px_h * pixel_size_y;

	float win_center_px_x = (float)(client_origin.x - disp_left) + (float)win_px_w / 2.0f;
	float win_center_px_y = (float)(client_origin.y - disp_top) + (float)win_px_h / 2.0f;
	float disp_center_px_x = (float)disp_px_w / 2.0f;
	float disp_center_px_y = (float)disp_px_h / 2.0f;

	out_metrics->window_center_offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
	out_metrics->window_center_offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

	out_metrics->valid = true;
	return true;
#elif defined(XRT_OS_MACOS)
	// Compute compositor-side from the live NSView — own window (hosted) or
	// the app's external view (handle). #524: the old code froze the window
	// at settings.preferred (the initial size) and assumed centered, so the
	// rig fov/canvas did not track live resize or window moves. Display info
	// comes from the DP when it implements pixel info, else from sys_info
	// (the VK sim DP implements neither on macOS). Mirrors the Metal
	// compositor's get_window_metrics + the Windows GetClientRect path above.
	if (c->macos_window == NULL) {
		return false;
	}

	uint32_t disp_px_w = 0, disp_px_h = 0;
	int32_t disp_left = 0, disp_top = 0;
	float disp_w_m = 0.0f, disp_h_m = 0.0f;
	bool have_disp = false;
	if (c->display_processor != NULL &&
	    xrt_display_processor_get_display_pixel_info(
	        c->display_processor, &disp_px_w, &disp_px_h,
	        &disp_left, &disp_top) &&
	    disp_px_w > 0 && disp_px_h > 0 &&
	    xrt_display_processor_get_display_dimensions(
	        c->display_processor, &disp_w_m, &disp_h_m) &&
	    disp_w_m > 0.0f && disp_h_m > 0.0f) {
		have_disp = true;
	}
	if (!have_disp && c->sys_info_set &&
	    c->sys_info.display_pixel_width > 0 && c->sys_info.display_pixel_height > 0 &&
	    c->sys_info.display_width_m > 0.0f && c->sys_info.display_height_m > 0.0f) {
		disp_px_w = c->sys_info.display_pixel_width;
		disp_px_h = c->sys_info.display_pixel_height;
		disp_left = c->sys_info.display_screen_left;
		disp_top = c->sys_info.display_screen_top;
		disp_w_m = c->sys_info.display_width_m;
		disp_h_m = c->sys_info.display_height_m;
		have_disp = true;
	}
	if (!have_disp) {
		return false;
	}

	uint32_t win_px_w = 0, win_px_h = 0;
	comp_vk_native_window_macos_get_dimensions(c->macos_window, &win_px_w, &win_px_h);
	if (win_px_w == 0 || win_px_h == 0) return false;

	float pixel_size_x = disp_w_m / (float)disp_px_w;
	float pixel_size_y = disp_h_m / (float)disp_px_h;

	out_metrics->display_width_m = disp_w_m;
	out_metrics->display_height_m = disp_h_m;
	out_metrics->display_pixel_width = disp_px_w;
	out_metrics->display_pixel_height = disp_px_h;
	out_metrics->display_screen_left = disp_left;
	out_metrics->display_screen_top = disp_top;

	out_metrics->window_pixel_width = win_px_w;
	out_metrics->window_pixel_height = win_px_h;

	out_metrics->window_width_m = (float)win_px_w * pixel_size_x;
	out_metrics->window_height_m = (float)win_px_h * pixel_size_y;

	// Window centre offset within the display: real on-screen position when
	// available (so window-relative 3D tracks window moves), else centred.
	float disp_center_px_x = (float)disp_px_w / 2.0f;
	float disp_center_px_y = (float)disp_px_h / 2.0f;
	float win_center_px_x = disp_center_px_x;
	float win_center_px_y = disp_center_px_y;
	int32_t win_left = 0, win_top = 0;
	if (comp_vk_native_window_macos_get_screen_position(
	        c->macos_window, &win_left, &win_top)) {
		win_center_px_x = (float)(win_left - disp_left) + (float)win_px_w / 2.0f;
		win_center_px_y = (float)(win_top - disp_top) + (float)win_px_h / 2.0f;
	}
	out_metrics->window_screen_left = win_left;
	out_metrics->window_screen_top = win_top;

	out_metrics->window_center_offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
	out_metrics->window_center_offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

	out_metrics->valid = true;
	return true;
#elif defined(XRT_OS_LINUX_DESKTOP)
	// Mirror the macOS path: window metrics from the live XCB window. Display
	// info from the DP (pixel info + dims) when available, else from sys_info;
	// window size + screen position from XCB (X11 exposes absolute window pos,
	// so window-relative 3D tracks window moves — the reason XCB precedes
	// Wayland in the Linux plan).
	//
	// Two window sources: the self-owned helper (hosted class, c->xcb_window)
	// and the app-provided window (handle class via XR_DXR_xlib_window_binding,
	// c->xcb_handle — no helper struct). WITHOUT this handle branch a handle app
	// returned no metrics, so the runtime fell back to DISPLAY-scoped Kooima
	// (#396 W7): a 0.6 m-panel-sized rig drawn into a small window ⟹ oversized
	// cube + oversized disparity. Mirror the live-resize path (which already
	// polls c->xcb_handle geometry).
	const bool have_app_window = (c->xcb_window == NULL && c->xcb_handle.connection != NULL);
	// Third window source (#817): app-provided Wayland surface. Wayland gives
	// us no window to query, so size + position come from the compositor's
	// geometry service instead of XCB.
#ifdef DXR_HAVE_WL_GEOM
	const bool have_wayland_geom = c->use_wayland && c->wl_geom != NULL;
#else
	const bool have_wayland_geom = false;
#endif
	if (c->xcb_window == NULL && !have_app_window && !have_wayland_geom) {
		return false;
	}

	uint32_t disp_px_w = 0, disp_px_h = 0;
	int32_t disp_left = 0, disp_top = 0;
	float disp_w_m = 0.0f, disp_h_m = 0.0f;
	bool have_disp = false;
	if (c->display_processor != NULL &&
	    xrt_display_processor_get_display_pixel_info(
	        c->display_processor, &disp_px_w, &disp_px_h,
	        &disp_left, &disp_top) &&
	    disp_px_w > 0 && disp_px_h > 0 &&
	    xrt_display_processor_get_display_dimensions(
	        c->display_processor, &disp_w_m, &disp_h_m) &&
	    disp_w_m > 0.0f && disp_h_m > 0.0f) {
		have_disp = true;
	}
	if (!have_disp && c->sys_info_set &&
	    c->sys_info.display_pixel_width > 0 && c->sys_info.display_pixel_height > 0 &&
	    c->sys_info.display_width_m > 0.0f && c->sys_info.display_height_m > 0.0f) {
		disp_px_w = c->sys_info.display_pixel_width;
		disp_px_h = c->sys_info.display_pixel_height;
		disp_left = c->sys_info.display_screen_left;
		disp_top = c->sys_info.display_screen_top;
		disp_w_m = c->sys_info.display_width_m;
		disp_h_m = c->sys_info.display_height_m;
		have_disp = true;
	}
	if (!have_disp) {
		return false;
	}

	uint32_t win_px_w = 0, win_px_h = 0;
#ifdef DXR_HAVE_WL_GEOM
	int32_t wlg_left = 0, wlg_top = 0;
	bool have_wlg_rect = false;
	if (have_wayland_geom) {
		float wlg_scale = 1.0f;
		have_wlg_rect = comp_vk_native_wl_geom_get_window_rect(c->wl_geom, &wlg_left, &wlg_top, &win_px_w,
		                                                       &win_px_h, &wlg_scale);
		if (!have_wlg_rect) {
			// Geometry service has no window for this process yet
			// (extension absent / window unmapped) — no metrics, the
			// caller stays display-scoped.
			return false;
		}
	} else
#endif
	    if (have_app_window) {
		comp_vk_native_window_xcb_query_geometry(&c->xcb_handle, &win_px_w, &win_px_h);
	} else {
		comp_vk_native_window_xcb_get_dimensions(c->xcb_window, &win_px_w, &win_px_h);
	}
	if (win_px_w == 0 || win_px_h == 0) return false;

	float pixel_size_x = disp_w_m / (float)disp_px_w;
	float pixel_size_y = disp_h_m / (float)disp_px_h;

	out_metrics->display_width_m = disp_w_m;
	out_metrics->display_height_m = disp_h_m;
	out_metrics->display_pixel_width = disp_px_w;
	out_metrics->display_pixel_height = disp_px_h;
	out_metrics->display_screen_left = disp_left;
	out_metrics->display_screen_top = disp_top;

	out_metrics->window_pixel_width = win_px_w;
	out_metrics->window_pixel_height = win_px_h;

	out_metrics->window_width_m = (float)win_px_w * pixel_size_x;
	out_metrics->window_height_m = (float)win_px_h * pixel_size_y;

	float disp_center_px_x = (float)disp_px_w / 2.0f;
	float disp_center_px_y = (float)disp_px_h / 2.0f;
	float win_center_px_x = disp_center_px_x;
	float win_center_px_y = disp_center_px_y;
	int32_t win_left = 0, win_top = 0;
	bool have_pos;
#ifdef DXR_HAVE_WL_GEOM
	if (have_wayland_geom) {
		// Mutter global coordinates == X11 root coordinates at scale 1.0,
		// so the display-origin subtraction below applies unchanged.
		win_left = wlg_left;
		win_top = wlg_top;
		have_pos = have_wlg_rect;
	} else
#endif
	{
		have_pos = have_app_window
		               ? comp_vk_native_window_xcb_query_screen_position(&c->xcb_handle, &win_left, &win_top)
		               : comp_vk_native_window_xcb_get_screen_position(c->xcb_window, &win_left, &win_top);
	}
	if (have_pos) {
		win_center_px_x = (float)(win_left - disp_left) + (float)win_px_w / 2.0f;
		win_center_px_y = (float)(win_top - disp_top) + (float)win_px_h / 2.0f;
	}
	out_metrics->window_screen_left = win_left;
	out_metrics->window_screen_top = win_top;

	out_metrics->window_center_offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
	out_metrics->window_center_offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

	out_metrics->valid = true;
	return true;
#else
	// Android: report the LIVE (orientation-aware) target surface extent in
	// pixels. The physical panel meters live in the runtime's xsysc->info (the
	// DP's get_display_dimensions is unreliable here — it can return 0), so we
	// populate pixels only and leave meters 0; the Kooima maps the physical
	// dims to this orientation (#499 portrait aspect). CNSDK weaves to the panel
	// regardless of orientation, so we only need the right aspect into Kooima.
	if (c->target == NULL) {
		return false;
	}
	uint32_t win_px_w = 0, win_px_h = 0;
	comp_vk_native_target_get_dimensions(c->target, &win_px_w, &win_px_h);
	if (win_px_w == 0 || win_px_h == 0) {
		return false;
	}
	out_metrics->window_pixel_width = win_px_w;
	out_metrics->window_pixel_height = win_px_h;
	out_metrics->display_pixel_width = win_px_w;   // fullscreen: window == display
	out_metrics->display_pixel_height = win_px_h;
	out_metrics->window_screen_left = 0;
	out_metrics->window_screen_top = 0;
	out_metrics->display_screen_left = 0;
	out_metrics->display_screen_top = 0;
	out_metrics->window_orientation = (struct xrt_quat){0, 0, 0, 1};
	// Meters left 0 (see above) — the caller derives them from xsysc->info.
	out_metrics->valid = true;

	/*
	 * PER-WINDOW Kooima, in-process (ADR-036 D6, epic #1031, #1034 in-process
	 * twin). With N windows on one panel every session composites into ITS OWN
	 * window, so the Kooima canvas is that window's physical rectangle — not
	 * the panel. Left display-scoped, a half-width window renders a frustum
	 * ~2x too wide whose off-axis skew is referenced to the panel centre, so
	 * two side-by-side cubes read as two independent centred views instead of
	 * one shared space seen through two apertures.
	 *
	 * Filling window_*_m + window_center_offset_* here is all it takes: the
	 * Kooima block in oxr_session.c already prefers those over the display
	 * dims, and rebases the render eyes to the window centre. Exactly the
	 * shape ipc_try_get_oop_view_poses uses out-of-process.
	 *
	 * Source is the rect the app publishes each frame via
	 * xrSetAndroidWindowGeometryDXR: physical screen pixels in the CURRENT
	 * rotation, y down, together with the panel extent in that same rotation.
	 * Both are needed — the runtime's own display info is the NATURAL-
	 * orientation panel (this device class is natively portrait and runs
	 * landscape) and a sub-panel window fits inside both orderings, so the
	 * held orientation is not recoverable from the rect alone. No rect (or a
	 * pre-#1037 app) → everything above stands unchanged.
	 */
	{
		int32_t win_x = 0, win_y = 0;
		uint32_t rect_w = 0, rect_h = 0, panel_w = 0, panel_h = 0;
		int32_t display_id = -1;
		uint64_t generation = 0;
		float pitch = 0.0f;
		if (c->sys_info_set && c->sys_info.display_pixel_width > 0 && c->sys_info.display_width_m > 0.0f) {
			// Square-pixel pitch from the NATIVE panel dims — orientation-
			// invariant, so it is valid against the current-rotation pixels.
			pitch = c->sys_info.display_width_m / (float)c->sys_info.display_pixel_width;
		}
		if (pitch > 0.0f && android_globals_get_window_screen_rect(&win_x, &win_y, &rect_w, &rect_h,
		                                                          &display_id, &panel_w, &panel_h,
		                                                          &generation) &&
		    rect_w > 0 && rect_h > 0 && panel_w > 0 && panel_h > 0) {
			out_metrics->window_pixel_width = rect_w;
			out_metrics->window_pixel_height = rect_h;
			out_metrics->display_pixel_width = panel_w;
			out_metrics->display_pixel_height = panel_h;
			out_metrics->window_screen_left = win_x;
			out_metrics->window_screen_top = win_y;
			out_metrics->display_width_m = (float)panel_w * pitch;
			out_metrics->display_height_m = (float)panel_h * pitch;
			out_metrics->window_width_m = (float)rect_w * pitch;
			out_metrics->window_height_m = (float)rect_h * pitch;

			const float win_center_x = (float)win_x + (float)rect_w * 0.5f;
			const float win_center_y = (float)win_y + (float)rect_h * 0.5f;
			// +x right in both frames; y is negated because screen pixels
			// are y-down and eye coordinates are y-up.
			out_metrics->window_center_offset_x_m = (win_center_x - (float)panel_w * 0.5f) * pitch;
			out_metrics->window_center_offset_y_m = -((win_center_y - (float)panel_h * 0.5f) * pitch);
			out_metrics->window_center_offset_z_m = 0.0f;

			// Lifecycle only (a window moved or resized), never per frame.
			if (generation != c->android_rect_generation_logged) {
				c->android_rect_generation_logged = generation;
				U_LOG_W("in-process Kooima: per-window rect=(%d,%d %ux%u)px panel=%ux%u "
				        "canvas=%.4fx%.4fm offset=(%.4f,%.4f)m (#1037/#1034)",
				        win_x, win_y, rect_w, rect_h, panel_w, panel_h,
				        (double)out_metrics->window_width_m,
				        (double)out_metrics->window_height_m,
				        (double)out_metrics->window_center_offset_x_m,
				        (double)out_metrics->window_center_offset_y_m);
			}
		}
	}
	return true;
#endif
}

/*!
 * Windowed weaving (runtime#757 / LeiaSR#85): tell the DP where the app window's
 * client area sits on the 3D panel so it can anchor the interlacing phase there,
 * instead of assuming the window covers the panel from its top-left. The panel
 * origin is the window's client top-left minus the display's screen origin — both
 * already computed by @ref comp_vk_native_compositor_get_window_metrics (the same
 * source the Kooima window metrics use, so the framing and the weave phase agree).
 *
 * No-op unless the DP exposes the (ADR-020) `set_present_origin` slot; a
 * full-panel window (hosted, Android) yields origin (0,0) = display-scoped =
 * today's behavior. Sticky on the DP side, but we refresh it every weave (cheap)
 * so a dragged/moved window keeps a correct phase.
 */
static void
vk_update_present_origin(struct comp_vk_native_compositor *c)
{
	if (c->display_processor == NULL) {
		return;
	}

#ifdef XRT_OS_ANDROID
	/*
	 * Android takes the D6 route instead of set_present_origin: the DP needs
	 * the window's full on-panel RECT (x, y, w, h, display_id), because a pure
	 * window MOVE raises no resize and SurfaceFlinger repositions the layer
	 * with the old buffer — nothing else in the pipeline can see it. Same feed
	 * the out-of-process path makes in comp_multi_system.c's
	 * update_window_screen_rect (#1033), from the same android_globals sink;
	 * in-process the publisher is the app's own
	 * xrSetAndroidWindowGeometryDXR (#1037).
	 *
	 * Sticky on the DP side, but re-asserted whenever the rect changes: a DP
	 * recreated mid-session then never misses the origin. No rect published →
	 * the DP stays display-scoped, exactly the behaviour that shipped before.
	 */
	{
		int32_t x = 0, y = 0, display_id = -1;
		uint32_t w = 0, h = 0, disp_w = 0, disp_h = 0;
		uint64_t generation = 0;
		if (android_globals_get_window_screen_rect(&x, &y, &w, &h, &display_id, &disp_w, &disp_h,
		                                           &generation)) {
			if (generation != c->android_rect_generation_fed) {
				c->android_rect_generation_fed = generation;
				// Lifecycle event (a window moved/resized), never per frame.
				U_LOG_W("WINDOW_RECT: in-process window %d,%d %ux%u display %d panel %ux%u "
				        "(#1037/#1033)",
				        x, y, w, h, display_id, disp_w, disp_h);
			}
			xrt_display_processor_vk_set_window_screen_rect(
			    (struct xrt_display_processor_vk *)c->display_processor, x, y, w, h, display_id);
		}
	}
	return;
#endif

	struct xrt_window_metrics m;
	if (!comp_vk_native_compositor_get_window_metrics(&c->base.base, &m) || !m.valid) {
		// No live window metrics (headless / no window) — leave the DP
		// display-scoped (its present origin defaults to (0,0)).
		return;
	}
	const int ox = m.window_screen_left - m.display_screen_left;
	const int oy = m.window_screen_top - m.display_screen_top;
	// Origin changed ⟹ the window is being dragged: have the target clamp
	// its bridge queue shallow so the weave phase sampled here is still
	// where the window IS when the frame reaches glass (#912 drag-shallow;
	// repro was 3D stutter on avatar RMB-move at governor depth 2-3).
	if (c->have_last_present_origin && c->target != NULL &&
	    (ox != c->last_present_origin_x || oy != c->last_present_origin_y)) {
		comp_vk_native_target_note_origin_motion(c->target);
	}
	c->last_present_origin_x = ox;
	c->last_present_origin_y = oy;
	c->have_last_present_origin = true;
	xrt_display_processor_vk_set_present_origin((struct xrt_display_processor_vk *)c->display_processor,
	                                            ox, oy);
}

bool
comp_vk_native_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d)
{
	if (xc == NULL) return false;
	struct comp_vk_native_compositor *c = vk_comp(xc);

	if (c->display_processor != NULL) {
		return xrt_display_processor_request_display_mode(c->display_processor, enable_3d);
	}
	return false;
}

void
comp_vk_native_compositor_set_eye_tracking_mode(struct xrt_compositor *xc, uint32_t mode)
{
	if (xc == NULL) return;
	struct comp_vk_native_compositor *c = vk_comp(xc);

	if (c->display_processor != NULL) {
		xrt_display_processor_set_eye_tracking_mode(c->display_processor, mode);
	}
}

void
comp_vk_native_compositor_set_system_devices(struct xrt_compositor *xc,
                                              struct xrt_system_devices *xsysd)
{
	if (xc == NULL) return;
	struct comp_vk_native_compositor *c = vk_comp(xc);

	c->xsysd = xsysd;

	if (xsysd != NULL) {
		U_LOG_I("VK native compositor: system devices set");
	}

	// macOS: no window-level input handling needed (uses oxr_macos event pump)

#ifdef XRT_OS_WINDOWS
	if (c->owns_window && c->own_window != NULL) {
		comp_d3d11_window_set_system_devices(c->own_window, xsysd);
	}
#endif
}

void
comp_vk_native_compositor_set_sys_info(struct xrt_compositor *xc,
                                        const struct xrt_system_compositor_info *info)
{
	if (xc == NULL || info == NULL) return;
	struct comp_vk_native_compositor *c = vk_comp(xc);
	c->sys_info = *info;
	c->sys_info_set = true;
	c->legacy_app_tile_scaling = info->legacy_app_tile_scaling;
	c->last_3d_mode_index = 1;

	// Legacy apps: fix view dims at the actual recommended size the app was told to render at.
	if (info->legacy_app_tile_scaling && c->renderer != NULL &&
	    info->legacy_view_width_pixels > 0 && info->legacy_view_height_pixels > 0) {
		uint32_t vw = info->legacy_view_width_pixels;
		uint32_t vh = info->legacy_view_height_pixels;
		uint32_t tc = 2, tr = 1;
		if (c->xdev != NULL && c->xdev->hmd != NULL) {
			uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
			if (idx < c->xdev->rendering_mode_count &&
			    c->xdev->rendering_modes[idx].tile_columns > 0) {
				tc = c->xdev->rendering_modes[idx].tile_columns;
				tr = c->xdev->rendering_modes[idx].tile_rows;
			}
		}
		// #868: reallocates the atlas the repaint replay holds a view of.
		// Same lock + disarm as begin_frame's resize path. Normally called
		// once at session setup, but nothing guarantees that.
		os_mutex_lock(&c->mutex);
		vk_repaint_disarm_locked(c);
		comp_vk_native_renderer_resize(c->renderer, vw, vh, tc * vw, tr * vh);
		os_mutex_unlock(&c->mutex);
	}
}

void
comp_vk_native_compositor_set_legacy_app_tile_scaling(struct xrt_compositor *xc, bool legacy)
{
	if (xc == NULL) return;
	struct comp_vk_native_compositor *c = vk_comp(xc);
	c->legacy_app_tile_scaling = legacy;
}

struct vk_bundle *
comp_vk_native_compositor_get_vk(struct comp_vk_native_compositor *c)
{
	return &c->vk;
}

uint32_t
comp_vk_native_compositor_get_queue_family(struct comp_vk_native_compositor *c)
{
	return c->queue_family_index;
}


/*
 *
 * XR_DXR_local_3d_zone — VK consumer leg (#439 Phase 3).
 *
 * Builds the masked-composite mechanism net-new in Vulkan (the D3D11 leg in
 * comp_d3d11_compositor.cpp is the line-by-line algorithm reference). The
 * authored R8 zone mask is rasterized GPU-side (one-shot cmd buffers, since
 * these run outside layer_commit) and snapshotted into a SAMPLED `staged`
 * sibling on submit; the per-frame composite (vk_composite_local_2d, defined
 * above near layer_commit) lerps M*weave + (1-M)*twod into the woven target.
 *
 */

//! Authored zone-mask handle (XR_DXR_local_3d_zone). `tex` is the R8 raster
//! target (COLOR_ATTACHMENT, also app-drawable for Tier-3); `staged` is the
//! SAMPLED snapshot the composite reads, decoupled so in-progress authoring
//! can't tear into a frame.
struct comp_vk_native_zone_mask
{
	uint32_t w, h;
	VkImage tex;
	VkDeviceMemory tex_mem;
	VkImageView tex_view;
	VkFramebuffer fb; //!< Over tex, with local2d.mask_rp.
	VkImage staged;
	VkDeviceMemory staged_mem;
	VkImageView staged_view;
	VkImageLayout tex_layout;
	bool submitted;
};

static const VkImageSubresourceRange k_color_sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

// One-shot cmd buffer for the cross-call zone-mask raster ops (create / set_* /
// submit run outside layer_commit). Record → submit → wait → free, matching the
// rest of this file's single-threaded GPU-op idiom.
static VkCommandBuffer
vk_oneshot_begin(struct comp_vk_native_compositor *c)
{
	struct vk_bundle *vk = &c->vk;
	VkCommandPool pool = (VkCommandPool)(uintptr_t)comp_vk_native_renderer_get_cmd_pool(c->renderer);
	VkCommandBufferAllocateInfo ai = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vk->vkAllocateCommandBuffers(vk->device, &ai, &cmd) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	VkCommandBufferBeginInfo bi = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->vkBeginCommandBuffer(cmd, &bi);
	return cmd;
}

static void
vk_oneshot_end(struct comp_vk_native_compositor *c, VkCommandBuffer cmd)
{
	struct vk_bundle *vk = &c->vk;
	VkCommandPool pool = (VkCommandPool)(uintptr_t)comp_vk_native_renderer_get_cmd_pool(c->renderer);
	vk->vkEndCommandBuffer(cmd);
	VkSubmitInfo si = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	};
	if (vk->vkQueueSubmit(vk->main_queue->queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS) {
		vk->vkQueueWaitIdle(vk->main_queue->queue);
	}
	vk->vkFreeCommandBuffers(vk->device, pool, 1, &cmd);
}

// Create/reuse an RT-or-sampled image (+view, +optional framebuffer), freeing
// the old one on a dims change. Returns true if usable.
static bool
vk_ensure_rt(struct comp_vk_native_compositor *c,
             VkImage *img,
             VkDeviceMemory *mem,
             VkImageView *view,
             VkFramebuffer *fb,
             uint32_t *cw,
             uint32_t *ch,
             uint32_t w,
             uint32_t h,
             VkFormat fmt,
             VkImageUsageFlags usage,
             VkRenderPass rp,
             const char *what)
{
	struct vk_bundle *vk = &c->vk;
	if (*img != VK_NULL_HANDLE && *cw == w && *ch == h) {
		return true;
	}
	if (fb != NULL && *fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, *fb, NULL);
		*fb = VK_NULL_HANDLE;
	}
	if (*view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, *view, NULL);
		*view = VK_NULL_HANDLE;
	}
	if (*img != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, *img, NULL);
		*img = VK_NULL_HANDLE;
	}
	if (*mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, *mem, NULL);
		*mem = VK_NULL_HANDLE;
	}
	*cw = 0;
	*ch = 0;

	VkExtent2D ext = {w, h};
	if (vk_create_image_simple(vk, ext, fmt, usage, mem, img) != VK_SUCCESS) {
		U_LOG_E("[local2d] %s: image alloc %ux%u failed", what, w, h);
		return false;
	}
	if (vk_create_view(vk, *img, VK_IMAGE_VIEW_TYPE_2D, fmt, k_color_sub, view) != VK_SUCCESS) {
		U_LOG_E("[local2d] %s: view failed", what);
		return false;
	}
	if (rp != VK_NULL_HANDLE && fb != NULL) {
		VkFramebufferCreateInfo fb_ci = {
		    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		    .renderPass = rp,
		    .attachmentCount = 1,
		    .pAttachments = view,
		    .width = w,
		    .height = h,
		    .layers = 1,
		};
		if (vk->vkCreateFramebuffer(vk->device, &fb_ci, NULL, fb) != VK_SUCCESS) {
			U_LOG_E("[local2d] %s: framebuffer failed", what);
			return false;
		}
	}
	*cw = w;
	*ch = h;
	return true;
}

static void
vk_destroy_rt(struct comp_vk_native_compositor *c,
              VkImage *img,
              VkDeviceMemory *mem,
              VkImageView *view,
              VkFramebuffer *fb)
{
	struct vk_bundle *vk = &c->vk;
	if (fb != NULL && *fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, *fb, NULL);
		*fb = VK_NULL_HANDLE;
	}
	if (*view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, *view, NULL);
		*view = VK_NULL_HANDLE;
	}
	if (*img != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, *img, NULL);
		*img = VK_NULL_HANDLE;
	}
	if (*mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, *mem, NULL);
		*mem = VK_NULL_HANDLE;
	}
}

// #439 Phase 2/3 effective canvas: an active mask or a Local2D-carrying frame
// supersedes the canvas output rect with the client-window rect (the weave
// region, composite region, and view dims share one authority). With neither,
// returns c->canvas verbatim so the no-mask path is unchanged.
static struct u_canvas_rect
vk_effective_canvas(struct comp_vk_native_compositor *c)
{
	// XR_DXR_display_zones: a zones frame spans the full client window by
	// definition (each zone rect is its own canvas; the output rect is
	// inert) — same supersede geometry as the mask/Local2D rules.
	if (!c->zones_frame && c->active_zone_mask == NULL && !c->local_2d_last_frame) {
		return (struct u_canvas_rect){0};
	}
	struct u_canvas_rect win = {0};
#ifdef XRT_OS_WINDOWS
	if (c->hwnd != NULL) {
		RECT r;
		if (GetClientRect((HWND)c->hwnd, &r) && r.right > 0 && r.bottom > 0) {
			win.valid = true;
			win.x = 0;
			win.y = 0;
			win.w = (uint32_t)r.right;
			win.h = (uint32_t)r.bottom;
			return win;
		}
	}
#endif
	return win; // invalid → existing full-target fallbacks
}

// Compute the window region inside the (worst-case-allocated) dst surface.
static void
vk_window_region(struct comp_vk_native_compositor *c, uint32_t dst_w, uint32_t dst_h, uint32_t *rw, uint32_t *rh)
{
	*rw = dst_w;
	*rh = dst_h;
#ifdef XRT_OS_WINDOWS
	if (c->hwnd != NULL) {
		RECT r;
		if (GetClientRect((HWND)c->hwnd, &r) && r.right > 0 && r.bottom > 0) {
			*rw = ((uint32_t)r.right < dst_w) ? (uint32_t)r.right : dst_w;
			*rh = ((uint32_t)r.bottom < dst_h) ? (uint32_t)r.bottom : dst_h;
		}
	}
#endif
}

// #491 part 3 — reset the Local2D descriptor pool at most once per frame. Both
// the pre-weave backdrop flatten and the post-weave overlay composite allocate
// sets from the one pool; vk_local2d_composite_begin_frame resets it, which
// would invalidate sets still referenced by a shared (un-submitted) cmd buffer
// if called twice. The per-frame guard is cleared at the top of layer_commit.
static void
vk_local2d_begin_frame_once(struct comp_vk_native_compositor *c)
{
	if (c->local2d_pool_reset_this_frame) {
		return;
	}
	vk_local2d_composite_begin_frame(&c->local2d, &c->vk);
	c->local2d_pool_reset_this_frame = true;
}

// Flatten one Local2D layer into @p target_fb (premultiplied), clamped to the
// region. Shared by the pre-weave backdrop flatten (#491 part 3, under-layers)
// and the post-weave overlay flatten (#439 Phase 3, over-layers) so the source
// geometry / flip / unpremult handling stays identical between the two.
static void
vk_flatten_one_local2d_layer(struct comp_vk_native_compositor *c,
                             VkCommandBuffer cmd,
                             VkFramebuffer target_fb,
                             struct comp_layer *layer,
                             uint32_t region_w,
                             uint32_t region_h)
{
	struct vk_bundle *vk = &c->vk;
	struct xrt_swapchain *sc = layer->sc_array[0];
	if (sc == NULL) {
		return;
	}
	uint32_t img_idx = layer->data.local_2d.sub.image_index;
	// sRGB-passthrough: sample the layer's own view (the projection path
	// samples the same). UNORM-sibling decode is a follow-up if a layer
	// ever uses an _SRGB swapchain.
	VkImageView src_view = (VkImageView)(uintptr_t)comp_vk_native_swapchain_get_image_view(sc, img_idx);
	if (src_view == VK_NULL_HANDLE) {
		return;
	}

	const struct xrt_rect *dr = &layer->data.local_2d.rect;
	int32_t dx = dr->offset.w, dy = dr->offset.h, dw = dr->extent.w, dh = dr->extent.h;
	if (dw <= 0 || dh <= 0) {
		return;
	}
	int32_t x0 = dx < 0 ? 0 : dx;
	int32_t y0 = dy < 0 ? 0 : dy;
	int32_t x1 = (dx + dw) > (int32_t)region_w ? (int32_t)region_w : (dx + dw);
	int32_t y1 = (dy + dh) > (int32_t)region_h ? (int32_t)region_h : (dy + dh);
	if (x1 <= x0 || y1 <= y0) {
		return;
	}
	float fx0 = (float)(x0 - dx) / (float)dw;
	float fy0 = (float)(y0 - dy) / (float)dh;
	float fx1 = (float)(x1 - dx) / (float)dw;
	float fy1 = (float)(y1 - dy) / (float)dh;

	struct xrt_normalized_rect nr = layer->data.local_2d.sub.norm_rect;
	if (nr.w <= 0.0f || nr.h <= 0.0f) {
		nr.x = 0.0f;
		nr.y = 0.0f;
		nr.w = 1.0f;
		nr.h = 1.0f;
	}
	float src_x = nr.x + nr.w * fx0;
	float src_w = nr.w * (fx1 - fx0);
	float src_y, src_h;
	if (layer->data.flip_y) {
		src_y = nr.y + nr.h * (1.0f - fy0);
		src_h = -(nr.h * (fy1 - fy0));
	} else {
		src_y = nr.y + nr.h * fy0;
		src_h = nr.h * (fy1 - fy0);
	}
	bool unpremult = (layer->data.flags & XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT) != 0;

	vk_local2d_composite_flatten_draw(&c->local2d, vk, cmd, target_fb, region_w, region_h, src_view, x0, y0,
	                                  (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), src_x, src_y, src_w, src_h,
	                                  unpremult);
}

// #491 part 3 — flatten the frame's 2D-UNDER Local2D layers (those BEFORE the
// projection in xrEndFrame list order) into backdrop_scratch (premultiplied),
// PRE-weave, and return its view + dims so the caller can hand it to the DP via
// xrt_display_processor_set_background_2d. The DP composites `backdrop over
// captured-desktop` as the under-3D background, so a semi-transparent backdrop
// reveals the desktop. Returns VK_NULL_HANDLE (out dims 0) when there are no
// under-layers (no projection, or all Local2D layers are over-layers) — the
// caller then clears the DP backdrop. Records into @p cmd only (does NOT
// submit); leaves backdrop_scratch in SHADER_READ_ONLY_OPTIMAL so it is
// DP-sampleable and outlives the process_atlas call (compositor-owned image).
static VkImageView
vk_flatten_backdrop_2d(struct comp_vk_native_compositor *c,
                       VkCommandBuffer cmd,
                       uint32_t dst_w,
                       uint32_t dst_h,
                       uint32_t *out_w,
                       uint32_t *out_h)
{
	struct vk_bundle *vk = &c->vk;
	*out_w = 0;
	*out_h = 0;

	if (!c->local2d_initialized || !c->local_2d_last_frame) {
		return VK_NULL_HANDLE;
	}

	// Under = Local2D layers BEFORE the projection in list order. No projection
	// ⟹ everything is an over-layer ⟹ no backdrop.
	int32_t proj_idx = -1;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		enum xrt_layer_type t = c->layer_accum.layers[i].data.type;
		if (t == XRT_LAYER_PROJECTION || t == XRT_LAYER_PROJECTION_DEPTH) {
			proj_idx = (int32_t)i;
			break;
		}
	}
	if (proj_idx < 0) {
		return VK_NULL_HANDLE;
	}
	bool have_under = false;
	for (int32_t i = 0; i < proj_idx; i++) {
		if (c->layer_accum.layers[i].data.type == XRT_LAYER_LOCAL_2D) {
			have_under = true;
			break;
		}
	}
	if (!have_under) {
		return VK_NULL_HANDLE;
	}

	uint32_t region_w, region_h;
	vk_window_region(c, dst_w, dst_h, &region_w, &region_h);
	if (region_w == 0 || region_h == 0) {
		return VK_NULL_HANDLE;
	}

	const VkFormat scratch_fmt = VK_FORMAT_B8G8R8A8_UNORM;
	if (!vk_ensure_rt(c, &c->backdrop_scratch, &c->backdrop_scratch_mem, &c->backdrop_scratch_view,
	                  &c->backdrop_scratch_fb, &c->backdrop_scratch_w, &c->backdrop_scratch_h, region_w,
	                  region_h, scratch_fmt,
	                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
	                      VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	                  c->local2d.flatten_rp, "backdrop scratch")) {
		return VK_NULL_HANDLE;
	}

	vk_local2d_begin_frame_once(c);

	// Clear transparent + → COLOR_ATTACHMENT (mirrors the local2d_scratch prep).
	vk_cmd_image_barrier_locked(vk, cmd, c->backdrop_scratch, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
	                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, k_color_sub);
	VkClearColorValue transparent = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}};
	vk->vkCmdClearColorImage(cmd, c->backdrop_scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &transparent, 1,
	                         &k_color_sub);
	vk_cmd_image_barrier_locked(vk, cmd, c->backdrop_scratch, VK_ACCESS_TRANSFER_WRITE_BIT,
	                            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                            k_color_sub);

	// Flatten ONLY the under-layers (before the projection) into the backdrop.
	for (int32_t i = 0; i < proj_idx; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];
		if (layer->data.type != XRT_LAYER_LOCAL_2D) {
			continue;
		}
		vk_flatten_one_local2d_layer(c, cmd, c->backdrop_scratch_fb, layer, region_w, region_h);
	}

	vk_cmd_image_barrier_locked(vk, cmd, c->backdrop_scratch, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                            VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, k_color_sub);

	static bool logged = false;
	if (!logged) {
		logged = true;
		U_LOG_W("VK #491 part3: flattened 2D-under backdrop %ux%u (handed to DP set_background_2d)",
		        region_w, region_h);
	}

	*out_w = region_w;
	*out_h = region_h;
	return c->backdrop_scratch_view;
}

// #439 Phase 3 — masked 2D-over-3D composite, POST-weave. The DP has woven the
// 3D into `dst`; this overlays the frame's 2D content where the zone mask says
// "2D". Runs only when the frame carries OVER Local2D layers (the `twod`
// source); under-layers (before the projection) are the DP backdrop and are
// handled pre-weave by vk_flatten_backdrop_2d. A frame whose only Local2D
// layers are under-layers has no over pixels and is skipped. Returns true if it
// composited (dst left in dst_outgoing) or false if it skipped (dst untouched,
// still in dst_incoming).
static bool
vk_composite_local_2d(struct comp_vk_native_compositor *c,
                      VkCommandBuffer cmd,
                      VkImage dst_image,
                      VkImageView dst_view,
                      uint32_t dst_w,
                      uint32_t dst_h,
                      VkImageLayout dst_incoming,
                      VkImageLayout dst_outgoing,
                      bool reuse_twod)
{
	struct vk_bundle *vk = &c->vk;

	// XR_DXR_display_zones: a zones frame ALWAYS runs the composite (the
	// feathered wish edge lerps the weave toward the 2D flatten even with
	// zero Local2D layers); the implicit-mask + sticky-mask rules are inert.
	const bool zones_frame = c->zones_frame;

	if (!c->local2d_initialized || (!c->local_2d_last_frame && !zones_frame) || dst_image == VK_NULL_HANDLE ||
	    dst_view == VK_NULL_HANDLE) {
		return false;
	}

	uint32_t region_w, region_h;
	vk_window_region(c, dst_w, dst_h, &region_w, &region_h);
	if (region_w == 0 || region_h == 0) {
		return false;
	}

	const VkFormat scratch_fmt = VK_FORMAT_B8G8R8A8_UNORM; // matches target (raw weave copy)
	const VkFormat mask_fmt = VK_FORMAT_R8_UNORM;

	// #491 part 3 — split Local2D layers by list order vs the projection. A
	// layer BEFORE the projection is a 2D-under backdrop (handled pre-weave by
	// vk_flatten_backdrop_2d → the DP); AFTER (or with no projection) it is a
	// 2D-over overlay handled here. is_over(i) == !(proj_idx >= 0 && i < proj_idx).
	int32_t proj_idx = -1;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		enum xrt_layer_type t = c->layer_accum.layers[i].data.type;
		if (t == XRT_LAYER_PROJECTION || t == XRT_LAYER_PROJECTION_DEPTH) {
			proj_idx = (int32_t)i;
			break;
		}
	}

	// Collect this frame's OVER Local2D layer rects (for the implicit mask) once
	// — under-layers are the backdrop, not part of the overlay mask. In zones
	// frames there is no under/over split (2D-under reserved in v1): every
	// Local2D layer flattens as 2D-over.
	struct xrt_rect rects[XRT_MAX_LAYERS];
	uint32_t rect_count = 0;
	for (uint32_t i = 0; i < c->layer_accum.layer_count && rect_count < XRT_MAX_LAYERS; i++) {
		if (c->layer_accum.layers[i].data.type != XRT_LAYER_LOCAL_2D) {
			continue;
		}
		if (!zones_frame && proj_idx >= 0 && (int32_t)i < proj_idx) {
			continue; // under-layer (backdrop) — skip
		}
		rects[rect_count++] = c->layer_accum.layers[i].data.local_2d.rect;
	}
	if (rect_count == 0 && !zones_frame) {
		return false; // only under-layers (or stale flag) — nothing to overlay
	}

	// A zones frame with no Local2D layers whose zones cover the WHOLE region has
	// mask == 1 everywhere, so the composite reduces to `weave` — the identity.
	// Running it anyway costs a full-region flatten + lerp per frame (measured
	// ~1.45 ms on an 811x1421 region, Arc iGPU) to produce a bit-identical target.
	if (rect_count == 0 && zones_frame) {
		bool covers_region = false;
		for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
			if (c->layer_accum.layers[i].data.type != XRT_LAYER_ZONE_3D) {
				continue;
			}
			const struct xrt_rect r = c->layer_accum.layers[i].data.zone_3d.rect;
			if (c->layer_accum.layers[i].data.zone_3d.feather_px > 0.0f) {
				covers_region = false; // a feathered edge is not the identity
				break;
			}
			if (r.offset.w <= 0 && r.offset.h <= 0 && r.extent.w >= (int32_t)region_w &&
			    r.extent.h >= (int32_t)region_h) {
				covers_region = true;
			}
		}
		if (covers_region) {
			return false;
		}
	}

	// XR_DXR_display_zones: the auto wish rasterizes from the ZONE rects.
	struct xrt_rect zone_rects[XRT_MAX_LAYERS];
	float zone_feathers[XRT_MAX_LAYERS]; // per-zone opt-in feather (runtime#800)
	bool any_feather = false;
	uint32_t zone_rect_count = 0;
	if (zones_frame) {
		for (uint32_t i = 0; i < c->layer_accum.layer_count && zone_rect_count < XRT_MAX_LAYERS; i++) {
			if (c->layer_accum.layers[i].data.type != XRT_LAYER_ZONE_3D) {
				continue;
			}
			zone_feathers[zone_rect_count] = c->layer_accum.layers[i].data.zone_3d.feather_px;
			if (zone_feathers[zone_rect_count] > 0.0f) {
				any_feather = true;
			}
			zone_rects[zone_rect_count++] = c->layer_accum.layers[i].data.zone_3d.rect;
		}
	}

	// Resolve the `twod` source: flatten the Local2D layers into local2d_scratch.
	if (!vk_ensure_rt(c, &c->local2d_scratch, &c->local2d_scratch_mem, &c->local2d_scratch_view,
	                  &c->local2d_scratch_fb, &c->local2d_scratch_w, &c->local2d_scratch_h, region_w,
	                  region_h, scratch_fmt,
	                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
	                      VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	                  c->local2d.flatten_rp, "local2d scratch")) {
		return false;
	}
	// Weave snapshot scratch (the lerp reads a copy; dst is RT≠SRV).
	if (!vk_ensure_rt(c, &c->weave_scratch, &c->weave_scratch_mem, &c->weave_scratch_view, NULL,
	                  &c->weave_scratch_w, &c->weave_scratch_h, region_w, region_h, scratch_fmt,
	                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_NULL_HANDLE,
	                  "local2d weave")) {
		return false;
	}

	vk_local2d_begin_frame_once(c);

	/*
	 * --- twod: clear local2d_scratch transparent, flatten layers, → SHADER_READ.
	 *
	 * #875 DEPOSIT half. This is the ONLY part of the composite that reads
	 * app-owned memory — vk_flatten_one_local2d_layer samples the app's Local2D
	 * swapchain images. It therefore runs on the app frame ONLY. A repaint
	 * (reuse_twod) skips it and lerps from whatever the last app frame left in
	 * local2d_scratch, which is compositor-owned and still valid.
	 *
	 * Re-running it on a repaint would sample images the app has since
	 * reacquired and redrawn — that is precisely the 2D-bubble flicker found on
	 * D3D11 and D3D12. Skipping the barriers too is not an optimisation but a
	 * correctness requirement: the scratch is already in SHADER_READ from the
	 * app frame, so re-issuing a COLOR_ATTACHMENT→SHADER_READ transition would
	 * declare a wrong oldLayout.
	 */
	if (!reuse_twod) {
		vk_cmd_image_barrier_locked(vk, cmd, c->local2d_scratch, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
		                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                            k_color_sub);
		VkClearColorValue transparent = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}};
		vk->vkCmdClearColorImage(cmd, c->local2d_scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                         &transparent, 1, &k_color_sub);
		vk_cmd_image_barrier_locked(vk, cmd, c->local2d_scratch, VK_ACCESS_TRANSFER_WRITE_BIT,
		                            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
		                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                            VK_PIPELINE_STAGE_TRANSFER_BIT,
		                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);

		for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
			struct comp_layer *layer = &c->layer_accum.layers[i];
			if (layer->data.type != XRT_LAYER_LOCAL_2D) {
				continue;
			}
			// #491 part 3 — under-layers are the DP backdrop (handled pre-weave);
			// the overlay flattens only over-layers. Zones frames have no
			// under/over split (2D-under reserved in v1).
			if (!zones_frame && proj_idx >= 0 && (int32_t)i < proj_idx) {
				continue;
			}
			vk_flatten_one_local2d_layer(c, cmd, c->local2d_scratch_fb, layer, region_w, region_h);
		}
		vk_cmd_image_barrier_locked(vk, cmd, c->local2d_scratch, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                            VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, k_color_sub);
	}

	// --- mask. Zones frame (XR_DXR_display_zones): the WISH — the explicit
	// frame wish (staged in-cmd; referenced-at-frame-end = consume current
	// authored state, no submit required) or the auto ring-feathered raster
	// from the zone rects (reusing the implicit-mask image — the implicit
	// rule is inert in zones frames). Legacy: explicit submitted mask wins;
	// else rasterize the implicit one.
	VkImageView mask_view = VK_NULL_HANDLE;
	struct comp_vk_native_zone_mask *emask = c->active_zone_mask;

	/*
	 * #868: a repaint REUSES the mask the last app frame resolved; it must not
	 * re-resolve it and must not touch the publish state.
	 *
	 * Two separate reasons, both learned on D3D12:
	 *  - The block below does not only rasterise a mask, it PUBLISHES one
	 *    (c->zone_wish_view / zone_wish_w/h / zone_publish_seq++). Those are
	 *    once-per-app-frame values that layer_commit resets at the top and
	 *    vk_sync_zone_mask_to_dp consumes at the bottom. Driving them at panel
	 *    rate from the repaint thread desynchronises that sideband publish.
	 *  - Re-resolving is also just wrong: the repaint replays RENDERING, not
	 *    STATE TRANSITIONS. See [[repaint-never-touches-app-owned-state]].
	 *
	 * D3D11 does this with c->repaint.mask_srv and D3D12 with reuse_mask; VK
	 * was the one backend still re-resolving on every replay, which is why its
	 * 2D band flickered once it finally had content to show.
	 */
	if (reuse_twod) {
		mask_view = (VkImageView)(uintptr_t)c->repaint.mask_view;
		if (mask_view == VK_NULL_HANDLE) {
			return false; // no app frame has resolved one yet — nothing to replay
		}
	} else {

		// XR_DXR_display_zones: the COMPOSITE mask is always the BINARY zone
		// raster — per ADR-027 the wish is HARDWARE-only and composition follows
		// zone geometry + alpha, so an explicit frame wish never gates blending
		// (it publishes below, verbatim). Binary, not ring-feathered: feathering
		// is a cosmetic opt-in (runtime#800) and never belonged in either the
		// composite default or the published wish. The zones composite draw runs
		// MODE_ZONES (twod + (1-a)·(M·weave)), so M only gates the weave and
		// Local2D overlays composite on top by their own alpha — a full-window
		// zone no longer multiplies its toast away, and the window-edge feather
		// ring artifact is gone.
		if (zones_frame) {
			if (zone_rect_count == 0) {
				return false; // defensive — a zones frame always has zones
			}
			if (!vk_ensure_rt(c, &c->implicit_mask_tex, &c->implicit_mask_mem, &c->implicit_mask_view,
			                  &c->implicit_mask_fb, &c->implicit_mask_w, &c->implicit_mask_h, region_w,
			                  region_h, mask_fmt,
			                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			                  c->local2d.mask_rp, "zone mask")) {
				return false;
			}
			vk_cmd_image_barrier_locked(vk, cmd, c->implicit_mask_tex, 0,
			                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
			                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);
			vk_local2d_composite_raster_mask(&c->local2d, vk, cmd, c->implicit_mask_fb, region_w, region_h,
			                                 0.0f, zone_rects, zone_rect_count, 1.0f);
			vk_cmd_image_barrier_locked(vk, cmd, c->implicit_mask_tex, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			                            VK_ACCESS_SHADER_READ_BIT,
			                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, k_color_sub);
			mask_view = c->implicit_mask_view;

			// Per-zone opt-in feather (XrDisplayZoneFeatherDXR, runtime#800):
			// the COMPOSITE samples a separately-rastered mask with each zone's
			// requested inward ramp; the binary raster above still publishes as
			// the hardware wish (cosmetics never enter the wish).
			if (any_feather) {
				if (vk_ensure_rt(c, &c->feather_mask_tex, &c->feather_mask_mem, &c->feather_mask_view,
				                 &c->feather_mask_fb, &c->feather_mask_w, &c->feather_mask_h, region_w,
				                 region_h, mask_fmt,
				                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				                 c->local2d.mask_rp, "zone feather mask")) {
					vk_cmd_image_barrier_locked(
					    vk, cmd, c->feather_mask_tex, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
					    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
					    k_color_sub);
					vk_local2d_composite_raster_mask_zones(&c->local2d, vk, cmd, c->feather_mask_fb,
					                                       region_w, region_h, zone_rects, zone_feathers,
					                                       zone_rect_count);
					vk_cmd_image_barrier_locked(
					    vk, cmd, c->feather_mask_tex, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
					    VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
					    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, k_color_sub);
					mask_view = c->feather_mask_view;
				} // ensure failure: fall back to the binary mask — hard edges, never a lost frame
			}
		}
		c->repaint.mask_view = (uint64_t)(uintptr_t)mask_view;
	}

	if (zones_frame && c->frame_wish != NULL && c->frame_wish->staged != VK_NULL_HANDLE &&
	    c->frame_wish->tex != VK_NULL_HANDLE) {
		struct comp_vk_native_zone_mask *fw = c->frame_wish;
		// Stage tex → staged inside this frame's cmd (the submit body,
		// inlined): in-progress authoring can't tear into the frame.
		vk_cmd_image_barrier_locked(vk, cmd, fw->tex, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                            VK_ACCESS_TRANSFER_READ_BIT,
		                            fw->tex_layout != VK_IMAGE_LAYOUT_UNDEFINED
		                                ? fw->tex_layout
		                                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                            VK_PIPELINE_STAGE_TRANSFER_BIT, k_color_sub);
		vk_cmd_image_barrier_locked(vk, cmd, fw->staged, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
		                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                            k_color_sub);
		VkImageCopy wish_copy = {
		    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		    .extent = {fw->w, fw->h, 1},
		};
		vk->vkCmdCopyImage(cmd, fw->tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fw->staged,
		                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &wish_copy);
		vk_cmd_image_barrier_locked(vk, cmd, fw->staged, VK_ACCESS_TRANSFER_WRITE_BIT,
		                            VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, k_color_sub);
		vk_cmd_image_barrier_locked(vk, cmd, fw->tex, VK_ACCESS_TRANSFER_READ_BIT,
		                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);
		fw->tex_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		// NOTE: mask_view stays the binary zone raster from above — the
		// explicit wish is PUBLISH-ONLY (ADR-027: hardware-only, never a
		// compositor blend gate).

		// P4 publish source + seq: the staged explicit wish. Bump the
		// generation on a source change (pointer flip; VK masks carry no
		// author generation, so a same-pointer re-author keeps its seq —
		// vendors treat same-seq as anchor-only updates).
		c->zone_wish_view = fw->staged_view;
		c->zone_wish_w = fw->w;
		c->zone_wish_h = fw->h;
		if (c->zone_frame_wish_last != fw) {
			c->zone_frame_wish_last = fw;
			c->zone_publish_seq++;
		}
	} else if (zones_frame) {
		// Composite mask already rastered (binary) above; this branch is now
		// PUBLISH-ONLY — the auto wish publishes that same binary raster.
		// Feathering removed from the published wish (runtime#800: the wish
		// is the app's to author and defaults binary; feather is a cosmetic
		// composite opt-in, not a hardware signal).

		// P4 publish source + seq: the auto raster. It re-records every
		// zones frame, but identical rect set + dims = identical content —
		// bump the generation only when something actually changed (or the
		// source flipped explicit -> auto).
		bool wish_dirty = c->zone_frame_wish_last != NULL || c->zone_wish_rect_count != zone_rect_count ||
		                  c->zone_wish_w != region_w || c->zone_wish_h != region_h;
		for (uint32_t i = 0; !wish_dirty && i < zone_rect_count; i++) {
			if (memcmp(&c->zone_wish_rects[i], &zone_rects[i], sizeof(zone_rects[i])) != 0) {
				wish_dirty = true;
			}
		}
		if (wish_dirty) {
			c->zone_frame_wish_last = NULL;
			memcpy(c->zone_wish_rects, zone_rects, sizeof(zone_rects[0]) * zone_rect_count);
			c->zone_wish_rect_count = zone_rect_count;
			c->zone_publish_seq++;
		}
		c->zone_wish_view = c->implicit_mask_view;
		c->zone_wish_w = region_w;
		c->zone_wish_h = region_h;
	} else if (emask != NULL && emask->submitted && emask->staged_view != VK_NULL_HANDLE) {
		mask_view = emask->staged_view; // already SHADER_READ from submit
	} else {
		if (!vk_ensure_rt(c, &c->implicit_mask_tex, &c->implicit_mask_mem, &c->implicit_mask_view,
		                  &c->implicit_mask_fb, &c->implicit_mask_w, &c->implicit_mask_h, region_w,
		                  region_h, mask_fmt,
		                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		                  c->local2d.mask_rp, "implicit mask")) {
			return false;
		}
		vk_cmd_image_barrier_locked(vk, cmd, c->implicit_mask_tex, 0,
		                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
		                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);
		// Inverse of set_rects: M=1 (keep weave) everywhere, M=0 inside the
		// Local2D rects (show the flattened 2D there).
		vk_local2d_composite_raster_mask(&c->local2d, vk, cmd, c->implicit_mask_fb, region_w, region_h,
		                                 1.0f, rects, rect_count, 0.0f);
		vk_cmd_image_barrier_locked(vk, cmd, c->implicit_mask_tex, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                            VK_ACCESS_SHADER_READ_BIT,
		                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, k_color_sub);
		mask_view = c->implicit_mask_view;
	}

	// #862 (generalises #858) — clip the snapshot + composite to the pixels
	// where the pass is NOT the identity. The weave already lives in the
	// target (weave_scratch is a copy OF it), so per mode, with an implicit
	// mask, outside every Local2D rect:
	//   ALPHA_OVER  out = twod + (1-twod.a)·weave        → weave (identity)
	//   LERP        out = M·weave + (1-M)·twod, M==1     → weave (identity)
	//   ZONES       out = twod + (1-twod.a)·(M·weave)    → weave only where
	//               M==1, i.e. INSIDE a zone. Outside every zone M==0 and the
	//               pass genuinely clears to 0, so this holds only when the
	//               zones cover the whole region (and no feather ramp softens
	//               an edge — a ramp is not the identity either).
	// An explicitly authored mask may be anything anywhere, so it never clips.
	// Under opaque present the same holds: the collapsed form is twod-over-
	// weave with α forced to 1, and the swapchain is ALPHA_MODE_IGNORE.
	// Cover of the non-identity pixels, in region space. Empty count => the
	// whole pass is the identity (subsumes #858); NULL => no clipping.
	VkRect2D clips[8];
	uint32_t clip_count = 0;
	bool clip_active = false;
	{
		const bool explicit_mask = (emask != NULL && emask->submitted &&
		                            emask->staged_view != VK_NULL_HANDLE) ||
		                           !debug_get_bool_option_local2d_clip();

		// Bounding box of this frame's 2D rects — never the identity in any mode.
		bool have_twod = false;
		VkRect2D twod_box = {{0, 0}, {0, 0}};
		if (rect_count > 0) {
			int32_t x0 = rects[0].offset.w, y0 = rects[0].offset.h;
			int32_t x1 = x0 + rects[0].extent.w, y1 = y0 + rects[0].extent.h;
			for (uint32_t i = 1; i < rect_count; i++) {
				const struct xrt_rect r = rects[i];
				if (r.offset.w < x0) x0 = r.offset.w;
				if (r.offset.h < y0) y0 = r.offset.h;
				if (r.offset.w + r.extent.w > x1) x1 = r.offset.w + r.extent.w;
				if (r.offset.h + r.extent.h > y1) y1 = r.offset.h + r.extent.h;
			}
			if (x0 < 0) x0 = 0;
			if (y0 < 0) y0 = 0;
			if (x1 > (int32_t)region_w) x1 = (int32_t)region_w;
			if (y1 > (int32_t)region_h) y1 = (int32_t)region_h;
			if (x1 > x0 && y1 > y0) {
				twod_box.offset.x = x0;
				twod_box.offset.y = y0;
				twod_box.extent.width = (uint32_t)(x1 - x0);
				twod_box.extent.height = (uint32_t)(y1 - y0);
				have_twod = true;
			}
		}

		if (!explicit_mask && zones_frame) {
			// Largest zone INTERIOR (inset by its own feather, since the ramp is
			// not the identity either) is the identity area. Its complement in
			// the region is at most four bands — which is what makes a feathered
			// zone clippable at all: the leftover is a ring, not a box.
			VkRect2D best = {{0, 0}, {0, 0}};
			uint64_t best_area = 0;
			for (uint32_t i = 0; i < zone_rect_count; i++) {
				const int32_t f = (int32_t)zone_feathers[i];
				int32_t sx0 = zone_rects[i].offset.w + f;
				int32_t sy0 = zone_rects[i].offset.h + f;
				int32_t sx1 = zone_rects[i].offset.w + zone_rects[i].extent.w - f;
				int32_t sy1 = zone_rects[i].offset.h + zone_rects[i].extent.h - f;
				if (sx0 < 0) sx0 = 0;
				if (sy0 < 0) sy0 = 0;
				if (sx1 > (int32_t)region_w) sx1 = (int32_t)region_w;
				if (sy1 > (int32_t)region_h) sy1 = (int32_t)region_h;
				if (sx1 <= sx0 || sy1 <= sy0) {
					continue;
				}
				const uint64_t area = (uint64_t)(sx1 - sx0) * (uint64_t)(sy1 - sy0);
				if (area > best_area) {
					best_area = area;
					best.offset.x = sx0;
					best.offset.y = sy0;
					best.extent.width = (uint32_t)(sx1 - sx0);
					best.extent.height = (uint32_t)(sy1 - sy0);
				}
			}
			if (best_area > 0) {
				const int32_t bx0 = best.offset.x, by0 = best.offset.y;
				const int32_t bx1 = bx0 + (int32_t)best.extent.width;
				const int32_t by1 = by0 + (int32_t)best.extent.height;
				// Four bands around the identity interior (any may be empty).
				const VkRect2D bands[4] = {
				    {{0, 0}, {region_w, (uint32_t)by0}},                                 // top
				    {{0, by1}, {region_w, (uint32_t)((int32_t)region_h - by1)}},          // bottom
				    {{0, by0}, {(uint32_t)bx0, (uint32_t)(by1 - by0)}},                   // left
				    {{bx1, by0}, {(uint32_t)((int32_t)region_w - bx1), (uint32_t)(by1 - by0)}}, // right
				};
				for (uint32_t i = 0; i < 4; i++) {
					if (bands[i].extent.width > 0 && bands[i].extent.height > 0) {
						clips[clip_count++] = bands[i];
					}
				}
				// Only the part of the 2D box INSIDE the interior needs its own
				// rect — the rest is already covered by the bands. Intersecting
				// also keeps every rect disjoint, which vkCmdCopyImage requires
				// of its regions.
				if (have_twod) {
					int32_t ix0 = (int32_t)twod_box.offset.x > bx0 ? twod_box.offset.x : bx0;
					int32_t iy0 = (int32_t)twod_box.offset.y > by0 ? twod_box.offset.y : by0;
					int32_t tx1 = twod_box.offset.x + (int32_t)twod_box.extent.width;
					int32_t ty1 = twod_box.offset.y + (int32_t)twod_box.extent.height;
					int32_t ix1 = tx1 < bx1 ? tx1 : bx1;
					int32_t iy1 = ty1 < by1 ? ty1 : by1;
					if (ix1 > ix0 && iy1 > iy0) {
						clips[clip_count].offset.x = ix0;
						clips[clip_count].offset.y = iy0;
						clips[clip_count].extent.width = (uint32_t)(ix1 - ix0);
						clips[clip_count].extent.height = (uint32_t)(iy1 - iy0);
						clip_count++;
					}
				}
				clip_active = true;
			}
		} else if (!explicit_mask && !zones_frame) {
			// ALPHA_OVER / implicit LERP: identity everywhere outside the 2D rects.
			if (have_twod) {
				clips[clip_count++] = twod_box;
			}
			clip_active = true;
		}

		// Only clip when it actually saves fill — a tiny identity interior would
		// cost four bands that redundantly cover the whole region.
		if (clip_active) {
			uint64_t covered = 0;
			for (uint32_t i = 0; i < clip_count; i++) {
				covered += (uint64_t)clips[i].extent.width * clips[i].extent.height;
			}
			const uint64_t full = (uint64_t)region_w * region_h;
			if (covered * 10u > full * 8u) { // >80% of the region: not worth it
				clip_active = false;
				clip_count = 0;
			}
		}

		// One-shot (NOT per-frame): what the cover resolved to and the geometry
		// that produced it — the first thing to check when the composite stage
		// costs more than the 2D content suggests it should.
		static bool clip_logged = false;
		if (!clip_logged) {
			clip_logged = true;
			if (clip_active) {
				uint64_t covered = 0;
				for (uint32_t i = 0; i < clip_count; i++) {
					covered += (uint64_t)clips[i].extent.width * clips[i].extent.height;
				}
				U_LOG_W("VK Local2D clip (#862): %u rect(s), %u%% of the %ux%u region",
				        clip_count,
				        (unsigned)(covered * 100u / ((uint64_t)region_w * region_h)), region_w,
				        region_h);
				for (uint32_t i = 0; i < clip_count; i++) {
					U_LOG_W("  clip[%u]: %d,%d %ux%u", i, clips[i].offset.x, clips[i].offset.y,
					        clips[i].extent.width, clips[i].extent.height);
				}
			} else {
				U_LOG_W("VK Local2D clip (#862): none, full %ux%u — reason: %s", region_w,
				        region_h,
				        explicit_mask ? "explicit authored mask" : "no worthwhile identity area");
			}
			for (uint32_t i = 0; i < rect_count; i++) {
				U_LOG_W("  Local2D rect[%u]: %d,%d %dx%d", i, rects[i].offset.w,
				        rects[i].offset.h, rects[i].extent.w, rects[i].extent.h);
			}
			for (uint32_t i = 0; i < zone_rect_count; i++) {
				U_LOG_W("  zone rect[%u]: %d,%d %dx%d feather=%.1f", i, zone_rects[i].offset.w,
				        zone_rects[i].offset.h, zone_rects[i].extent.w, zone_rects[i].extent.h,
				        (double)zone_feathers[i]);
			}
		}
		if (clip_active && clip_count == 0) {
			return false; // every pixel is the identity — skip the pass (#858)
		}
	}

	// --- weave snapshot: dst → TRANSFER_SRC, copy region → weave_scratch, dst
	// → COLOR_ATTACHMENT for the composite render pass.
	vk_cmd_image_barrier_locked(vk, cmd, dst_image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                            VK_ACCESS_TRANSFER_READ_BIT, dst_incoming,
	                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
	                            VK_PIPELINE_STAGE_TRANSFER_BIT, k_color_sub);
	vk_cmd_image_barrier_locked(vk, cmd, c->weave_scratch, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
	                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                            k_color_sub);
	// #862: snapshot only the clipped sub-rect — the shader samples the scratch
	// 1:1 in region space and only reads inside the scissor, so the rest of the
	// scratch is never touched. Same offsets on both sides keeps that 1:1.
	// #862: snapshot only the rects the composite will actually read. The
	// shader samples the scratch 1:1 in region space and only inside its
	// scissor, so untouched scratch is never read. The clip rects are disjoint
	// by construction, which vkCmdCopyImage requires of its regions.
	VkImageCopy copies[8];
	uint32_t copy_count = 0;
	if (clip_active) {
		for (uint32_t i = 0; i < clip_count; i++) {
			copies[copy_count].srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			copies[copy_count].srcOffset = (VkOffset3D){clips[i].offset.x, clips[i].offset.y, 0};
			copies[copy_count].dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			copies[copy_count].dstOffset = (VkOffset3D){clips[i].offset.x, clips[i].offset.y, 0};
			copies[copy_count].extent =
			    (VkExtent3D){clips[i].extent.width, clips[i].extent.height, 1};
			copy_count++;
		}
	} else {
		copies[copy_count].srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copies[copy_count].srcOffset = (VkOffset3D){0, 0, 0};
		copies[copy_count].dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copies[copy_count].dstOffset = (VkOffset3D){0, 0, 0};
		copies[copy_count].extent = (VkExtent3D){region_w, region_h, 1};
		copy_count++;
	}
	vk->vkCmdCopyImage(cmd, dst_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->weave_scratch,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy_count, copies);
	vk_cmd_image_barrier_locked(vk, cmd, c->weave_scratch, VK_ACCESS_TRANSFER_WRITE_BIT,
	                            VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, k_color_sub);
	vk_cmd_image_barrier_locked(vk, cmd, dst_image, VK_ACCESS_TRANSFER_READ_BIT,
	                            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);

	// --- composite target framebuffer (over the rotating dst view).
	if (c->composite_target_fb_view != dst_view || c->composite_fb_w != dst_w ||
	    c->composite_fb_h != dst_h) {
		if (c->composite_target_fb != VK_NULL_HANDLE) {
			vk->vkDestroyFramebuffer(vk->device, c->composite_target_fb, NULL);
			c->composite_target_fb = VK_NULL_HANDLE;
		}
		VkFramebufferCreateInfo fb_ci = {
		    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		    .renderPass = c->local2d.composite_rp,
		    .attachmentCount = 1,
		    .pAttachments = &dst_view,
		    .width = dst_w,
		    .height = dst_h,
		    .layers = 1,
		};
		if (vk->vkCreateFramebuffer(vk->device, &fb_ci, NULL, &c->composite_target_fb) != VK_SUCCESS) {
			return false;
		}
		c->composite_target_fb_view = dst_view;
		c->composite_fb_w = dst_w;
		c->composite_fb_h = dst_h;
	}

	// Effective canvas (window rect while active) — carried into the CB; the
	// mask-lerp path ignores it, kept coherent anyway.
	struct u_canvas_rect ec = vk_effective_canvas(c);
	int32_t cx = ec.valid ? ec.x : 0;
	int32_t cy = ec.valid ? ec.y : 0;
	uint32_t cw = ec.valid ? ec.w : region_w;
	uint32_t ch = ec.valid ? ec.h : region_h;

	// #491: the implicit (auto) mask composites the 2D over the weave by its own
	// premultiplied alpha — translucent 2D reveals the 3D scene, not the desktop.
	// An explicit authored mask keeps the hard M-lerp (designer cutout/portal).
	// XR_DXR_display_zones: MODE_ZONES (twod + (1−a)·(M·weave)) — the binary
	// zone raster gates only the WEAVE; Local2D content composites on top by
	// its own alpha (ADR-027: the wish is hardware-only; composition follows
	// zone geometry + alpha). Formerly the hard M-lerp, which multiplied
	// overlays away inside zones and dimmed the feathered edge.
	const bool have_explicit = (emask != NULL && emask->submitted && emask->staged_view != VK_NULL_HANDLE);
	uint32_t composite_mode;
	if (zones_frame) {
		composite_mode = VK_LOCAL2D_COMPOSITE_MODE_ZONES;
	} else if (have_explicit) {
		composite_mode = VK_LOCAL2D_COMPOSITE_MODE_LERP;
	} else {
		composite_mode = VK_LOCAL2D_COMPOSITE_MODE_ALPHA_OVER;
	}
#if defined(XRT_OS_MACOS)
	// #568 macOS flat-2D-over-desktop: on the alpha-native transparent path the
	// desktop shows through wherever the final alpha is 0 (the non-opaque
	// CAMetalLayer composites our premultiplied pixels) — there is NO captured
	// desktop woven under the 2D (that is the Windows WGC 2D-under path). The
	// #491 alpha_over composite (frag = twod + (1-twod.a)·weave) reveals the
	// WEAVE under a transparent flat-2D pixel, so a Local2D zone placed over an
	// unrendered/transparent-intended projection band instead exposes whatever
	// the weave holds there (e.g. an app's uninitialized tile) as an opaque
	// fill — never the desktop. Fall back to the hard M-lerp
	// (frag = M·weave + (1-M)·twod): the implicit mask is M=1 outside the
	// Local2D rects (keep the weave) and M=0 inside them, so a flat-2D region
	// composites 2D-over-transparent — transparent 2D → alpha 0 → desktop,
	// mirroring how the projection layer's own transparency already reaches the
	// CAMetalLayer. Gated to the alpha-native + transparent-background case so
	// opaque presents and chroma-key DPs are untouched; explicit/zones masks
	// already use the hard lerp. Windows keeps alpha_over (its 2D-under path
	// supplies the captured desktop), so no cross-platform regression.
	if (composite_mode == VK_LOCAL2D_COMPOSITE_MODE_ALPHA_OVER && c->transparent_background &&
	    c->display_processor != NULL && xrt_display_processor_is_alpha_native(c->display_processor)) {
		composite_mode = VK_LOCAL2D_COMPOSITE_MODE_LERP;
	}
#endif
	// #833/#116 — opaque present on a transparent session: DWM completes no
	// blends, so the composite flattens against the weave (which the DP's
	// flattened gate already completed against the captured desktop) and
	// emits α=1. Opaque sessions keep today's behavior even with the env set.
	const bool opaque_present = c->transparent_background && debug_get_bool_option_present_opaque();
	vk_local2d_composite_draw(&c->local2d, vk, cmd, c->composite_target_fb, dst_w, dst_h,
	                          c->local2d_scratch_view, mask_view, c->weave_scratch_view, region_w,
	                          region_h, cx, cy, cw, ch, composite_mode, opaque_present,
	                          clip_active ? clips : NULL, clip_count);

	// One-shot lifecycle log (NOT per-frame): proves the masked composite ran +
	// which mask source resolved. WARN so it survives the hot-path INFO filter.
	{
		static bool logged = false;
		if (!logged) {
			logged = true;
			U_LOG_W("VK Local2D composite: %ux%u region, %u layer(s), %s mask (mode=%u)",
			        region_w, region_h, rect_count, have_explicit ? "explicit" : "implicit",
			        composite_mode);
		}
	}

	// dst → outgoing for the downstream stage (HUD / present / readback).
	vk_cmd_image_barrier_locked(vk, cmd, dst_image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
	                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, dst_outgoing,
	                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, k_color_sub);

	// Composite-tap diagnostics (#833 debugging): stash this frame's images
	// for the end-of-commit trigger dump.
	c->tap_target_image = dst_image;
	c->tap_target_layout = dst_outgoing;
	c->tap_target_w = dst_w;
	c->tap_target_h = dst_h;
	c->tap_region_w = region_w;
	c->tap_region_h = region_h;
	return true;
}

// Release all #439 Phase 3 composite + zone state (called from destroy).
static void
vk_release_local2d_state(struct comp_vk_native_compositor *c)
{
	struct vk_bundle *vk = &c->vk;
	if (c->composite_target_fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, c->composite_target_fb, NULL);
		c->composite_target_fb = VK_NULL_HANDLE;
		c->composite_target_fb_view = VK_NULL_HANDLE;
	}
	vk_destroy_rt(c, &c->local2d_scratch, &c->local2d_scratch_mem, &c->local2d_scratch_view,
	              &c->local2d_scratch_fb);
	vk_destroy_rt(c, &c->backdrop_scratch, &c->backdrop_scratch_mem, &c->backdrop_scratch_view,
	              &c->backdrop_scratch_fb);
	vk_destroy_rt(c, &c->weave_scratch, &c->weave_scratch_mem, &c->weave_scratch_view, NULL);
	vk_destroy_rt(c, &c->implicit_mask_tex, &c->implicit_mask_mem, &c->implicit_mask_view,
	              &c->implicit_mask_fb);
	vk_destroy_rt(c, &c->feather_mask_tex, &c->feather_mask_mem, &c->feather_mask_view,
	              &c->feather_mask_fb);
	if (c->local2d_initialized) {
		vk_local2d_composite_fini(&c->local2d, vk);
		c->local2d_initialized = false;
	}
}

// #224 / ADR-027 hardware-DP zone leg (P4) — one-time DP zone-capability
// probe, cached on the compositor. Returns true when the DP consumes
// published zone masks; caps are then in c->zone_dp_caps.
static bool
vk_zone_dp_supported(struct comp_vk_native_compositor *c)
{
	if (c->display_processor == NULL) {
		return false;
	}
	if (c->zone_dp_state == 0) { // 0 = unqueried, 1 = supported, 2 = legacy
		struct xrt_dp_local_zone_caps caps = {0};
		caps.struct_size = sizeof(caps);
		bool ok = xrt_display_processor_get_local_zone_caps(c->display_processor, &caps);
		c->zone_dp_state = (ok && caps.supported != 0) ? 1 : 2;
		if (c->zone_dp_state == 1) {
			c->zone_dp_caps = caps;
			U_LOG_W("VK zone DP: local zones supported, grid %ux%u max_mask %ux%u max_hz %u "
			        "wish_fractional=%u granularity=%u",
			        caps.zone_grid_width, caps.zone_grid_height, caps.max_mask_width,
			        caps.max_mask_height, caps.max_update_hz, caps.wish_fractional,
			        caps.switch_granularity);
		}
	}
	return c->zone_dp_state == 1;
}

// Keep the DP's view of this client's zone mask in sync with the
// compositor's — the VK clone of d3d11_sync_zone_mask_to_dp. Called once per
// layer_commit AFTER the frame's queue submit + wait-idle, so whatever view
// we hand over is GPU-complete and in SHADER_READ_ONLY_OPTIMAL (the publish
// contract). Zones frame: the WISH this frame's composite resolved
// (explicit staged view or the auto raster); legacy frame: the sticky
// submitted mask (new on VK in P4 — parity with d3d11). No resolvable
// source (mask destroyed, shared-image zones frame with no composite,
// teardown) drives the clear-on-deactivate edge, once.
static void
vk_sync_zone_mask_to_dp(struct comp_vk_native_compositor *c)
{
	if (!vk_zone_dp_supported(c)) {
		return; // legacy DP — tier-1 global fallback path unchanged.
	}

	VkImageView view = VK_NULL_HANDLE;
	uint32_t mask_w = 0;
	uint32_t mask_h = 0;
	if (c->zones_frame) {
		view = c->zone_wish_view;
		mask_w = c->zone_wish_w;
		mask_h = c->zone_wish_h;
	} else {
		struct comp_vk_native_zone_mask *mask = c->active_zone_mask;
		if (mask != NULL && mask->submitted && mask->staged_view != VK_NULL_HANDLE) {
			view = mask->staged_view;
			mask_w = mask->w;
			mask_h = mask->h;
		}
	}

	if (view == VK_NULL_HANDLE) {
		if (c->zone_published) {
			xrt_display_processor_clear_local_zone_mask(c->display_processor);
			c->zone_published = false;
		}
		return;
	}

#ifdef XRT_OS_WINDOWS
	// Screen-anchor the mask: client-area origin in physical screen pixels
	// (same HWND clamp as vk_window_region). No HWND → nothing to anchor
	// to; skip the publish.
	HWND wnd = (HWND)c->hwnd;
	RECT r;
	POINT origin = {0, 0};
	if (wnd == NULL || !GetClientRect(wnd, &r) || r.right <= 0 || r.bottom <= 0 || !ClientToScreen(wnd, &origin)) {
		return;
	}

	bool ok = xrt_display_processor_publish_local_zone_mask(c->display_processor, view, mask_w, mask_h,
	                                                        (int32_t)origin.x, (int32_t)origin.y,
	                                                        (uint32_t)r.right, (uint32_t)r.bottom,
	                                                        c->zone_publish_seq);
	if (ok) {
		c->zone_published = true;
	}
#else
	// No screen-anchor helper on the macOS/Android VK paths yet — skip the
	// publish (Windows-first; the clear edge above still runs).
	(void)mask_w;
	(void)mask_h;
#endif
}

xrt_result_t
comp_vk_native_compositor_zone_mask_create(struct xrt_compositor *xc, uint32_t w, uint32_t h, void **out_mask)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;
	if (out_mask == NULL || !c->local2d_initialized) {
		return XRT_ERROR_ALLOCATION;
	}

	// 0 → runtime chooses the client-window dims (the mask is window-sized).
	if (w == 0 || h == 0) {
#ifdef XRT_OS_WINDOWS
		if (c->hwnd != NULL) {
			RECT r;
			if (GetClientRect((HWND)c->hwnd, &r) && r.right > 0 && r.bottom > 0) {
				w = (uint32_t)r.right;
				h = (uint32_t)r.bottom;
			}
		}
#endif
		if (w == 0 || h == 0) {
			w = c->settings.preferred.width;
			h = c->settings.preferred.height;
		}
	}
	if (w == 0 || h == 0) {
		U_LOG_E("zone_mask_create: no window/surface to derive mask dims from");
		return XRT_ERROR_ALLOCATION;
	}

	struct comp_vk_native_zone_mask *mask = U_TYPED_CALLOC(struct comp_vk_native_zone_mask);
	if (mask == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	mask->w = w;
	mask->h = h;
	mask->tex_layout = VK_IMAGE_LAYOUT_UNDEFINED;

	uint32_t tw = 0, th = 0, sw = 0, sh = 0;
	bool ok = vk_ensure_rt(c, &mask->tex, &mask->tex_mem, &mask->tex_view, &mask->fb, &tw, &th, w, h,
	                       VK_FORMAT_R8_UNORM,
	                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
	                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	                       c->local2d.mask_rp, "zone_mask tex");
	VkFramebuffer no_fb = VK_NULL_HANDLE;
	ok = ok && vk_ensure_rt(c, &mask->staged, &mask->staged_mem, &mask->staged_view, &no_fb, &sw, &sh, w, h,
	                        VK_FORMAT_R8_UNORM,
	                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_NULL_HANDLE,
	                        "zone_mask staged");
	if (!ok) {
		vk_destroy_rt(c, &mask->staged, &mask->staged_mem, &mask->staged_view, NULL);
		vk_destroy_rt(c, &mask->tex, &mask->tex_mem, &mask->tex_view, &mask->fb);
		free(mask);
		return XRT_ERROR_ALLOCATION;
	}

	// Default all-3D (M=1): an unauthored-but-submitted mask degrades to the
	// full weave, never a blanked canvas.
	VkCommandBuffer cmd = vk_oneshot_begin(c);
	if (cmd != VK_NULL_HANDLE) {
		vk_cmd_image_barrier_locked(vk, cmd, mask->tex, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);
		vk_local2d_composite_raster_mask(&c->local2d, vk, cmd, mask->fb, w, h, 1.0f, NULL, 0, 1.0f);
		vk_oneshot_end(c, cmd);
		mask->tex_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	U_LOG_W("zone_mask_create: %ux%u (client-window px)", w, h);
	*out_mask = mask;
	return XRT_SUCCESS;
}

xrt_result_t
comp_vk_native_compositor_zone_mask_set_whole(struct xrt_compositor *xc, void *mask_ptr, bool enable_3d)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;
	struct comp_vk_native_zone_mask *mask = (struct comp_vk_native_zone_mask *)mask_ptr;
	if (mask == NULL || mask->fb == VK_NULL_HANDLE) {
		return XRT_ERROR_ALLOCATION;
	}
	VkCommandBuffer cmd = vk_oneshot_begin(c);
	if (cmd == VK_NULL_HANDLE) {
		return XRT_ERROR_VULKAN;
	}
	vk_cmd_image_barrier_locked(vk, cmd, mask->tex, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                            mask->tex_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);
	vk_local2d_composite_raster_mask(&c->local2d, vk, cmd, mask->fb, mask->w, mask->h,
	                                 enable_3d ? 1.0f : 0.0f, NULL, 0, 0.0f);
	mask->tex_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	vk_oneshot_end(c, cmd);
	return XRT_SUCCESS;
}

xrt_result_t
comp_vk_native_compositor_zone_mask_set_rects(struct xrt_compositor *xc,
                                              void *mask_ptr,
                                              uint32_t count,
                                              const struct xrt_rect *rects)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;
	struct comp_vk_native_zone_mask *mask = (struct comp_vk_native_zone_mask *)mask_ptr;
	if (mask == NULL || mask->fb == VK_NULL_HANDLE || (count > 0 && rects == NULL)) {
		return XRT_ERROR_ALLOCATION;
	}
	VkCommandBuffer cmd = vk_oneshot_begin(c);
	if (cmd == VK_NULL_HANDLE) {
		return XRT_ERROR_VULKAN;
	}
	vk_cmd_image_barrier_locked(vk, cmd, mask->tex, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                            mask->tex_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);
	// M=0 everywhere, then M=1 inside each rect.
	vk_local2d_composite_raster_mask(&c->local2d, vk, cmd, mask->fb, mask->w, mask->h, 0.0f, rects, count,
	                                 1.0f);
	mask->tex_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	vk_oneshot_end(c, cmd);
	return XRT_SUCCESS;
}

xrt_result_t
comp_vk_native_compositor_zone_mask_acquire_rt(struct xrt_compositor *xc,
                                               void *mask_ptr,
                                               void **out_image,
                                               void **out_image_view,
                                               uint32_t *out_w,
                                               uint32_t *out_h)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;
	struct comp_vk_native_zone_mask *mask = (struct comp_vk_native_zone_mask *)mask_ptr;
	if (mask == NULL || mask->tex == VK_NULL_HANDLE || out_image == NULL || out_image_view == NULL ||
	    out_w == NULL || out_h == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	// Tier-3: hand the R8 raster image to the app to draw into. Put it in
	// COLOR_ATTACHMENT_OPTIMAL (where the app's own render pass expects it);
	// submit then snapshots it to staged.
	VkCommandBuffer cmd = vk_oneshot_begin(c);
	if (cmd != VK_NULL_HANDLE) {
		vk_cmd_image_barrier_locked(vk, cmd, mask->tex, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                            mask->tex_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);
		vk_oneshot_end(c, cmd);
		mask->tex_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}
	*out_image = (void *)(uintptr_t)mask->tex;
	*out_image_view = (void *)(uintptr_t)mask->tex_view;
	*out_w = mask->w;
	*out_h = mask->h;
	return XRT_SUCCESS;
}

xrt_result_t
comp_vk_native_compositor_zone_mask_submit(struct xrt_compositor *xc, void *mask_ptr)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;
	struct comp_vk_native_zone_mask *mask = (struct comp_vk_native_zone_mask *)mask_ptr;
	if (mask == NULL || mask->tex == VK_NULL_HANDLE || mask->staged == VK_NULL_HANDLE) {
		return XRT_ERROR_ALLOCATION;
	}
	VkCommandBuffer cmd = vk_oneshot_begin(c);
	if (cmd == VK_NULL_HANDLE) {
		return XRT_ERROR_VULKAN;
	}
	// Snapshot tex → staged so in-progress authoring can't tear into a frame.
	vk_cmd_image_barrier_locked(vk, cmd, mask->tex, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                            VK_ACCESS_TRANSFER_READ_BIT, mask->tex_layout,
	                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                            VK_PIPELINE_STAGE_TRANSFER_BIT, k_color_sub);
	vk_cmd_image_barrier_locked(vk, cmd, mask->staged, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
	                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                            k_color_sub);
	VkImageCopy copy = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .extent = {mask->w, mask->h, 1},
	};
	vk->vkCmdCopyImage(cmd, mask->tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mask->staged,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	vk_cmd_image_barrier_locked(vk, cmd, mask->staged, VK_ACCESS_TRANSFER_WRITE_BIT,
	                            VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, k_color_sub);
	// tex returns to COLOR_ATTACHMENT for the next authoring round.
	vk_cmd_image_barrier_locked(vk, cmd, mask->tex, VK_ACCESS_TRANSFER_READ_BIT,
	                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);
	vk_oneshot_end(c, cmd);

	mask->tex_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	mask->submitted = true;
	c->active_zone_mask = mask; // sticky last-submit-wins
	c->zone_publish_seq++;      // #224 P4: new content generation for the DP publish
	return XRT_SUCCESS;
}

void
comp_vk_native_compositor_zone_mask_destroy(struct xrt_compositor *xc, void *mask_ptr)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;
	struct comp_vk_native_zone_mask *mask = (struct comp_vk_native_zone_mask *)mask_ptr;
	if (mask == NULL) {
		return;
	}
	vk->vkDeviceWaitIdle(vk->device); // mask may be in flight
	if (c->active_zone_mask == mask) {
		c->active_zone_mask = NULL; // revert to implicit / legacy behavior
	}
	// XR_DXR_display_zones: never leave a dangling frame-wish reference.
	if (c->frame_wish == mask) {
		c->frame_wish = NULL;
	}
	// #224 P4: drop the seq-dedup cache (pointer may be reused by a future
	// alloc) and any per-frame wish view borrowed from this mask.
	if (c->zone_frame_wish_last == mask) {
		c->zone_frame_wish_last = NULL;
	}
	if (c->zone_wish_view == mask->staged_view) {
		c->zone_wish_view = VK_NULL_HANDLE;
	}
	vk_destroy_rt(c, &mask->staged, &mask->staged_mem, &mask->staged_view, NULL);
	vk_destroy_rt(c, &mask->tex, &mask->tex_mem, &mask->tex_view, &mask->fb);
	free(mask);
}

void
comp_vk_native_compositor_zones_set_frame_wish(struct xrt_compositor *xc, void *mask)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

	// Per-frame reference (XR_DXR_display_zones): oxr sets this on every
	// zones frame before layer_commit, NULL meaning auto-derive. Consumed
	// by the commit's composite; harmlessly stale on zero-zone frames (the
	// zones branch never reads it there).
	c->frame_wish = (struct comp_vk_native_zone_mask *)mask;
}

// #439 Phase 3 Q4 — current recommended per-view render size (the renderer's
// view dims, recomputed each frame from the effective canvas). oxr fires
// XrEventDataLocal3DZoneViewSizeChangedDXR when this changes.
bool
comp_vk_native_compositor_get_recommended_view_size(struct xrt_compositor *xc, uint32_t *out_w, uint32_t *out_h)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	if (out_w == NULL || out_h == NULL || c->renderer == NULL) {
		return false;
	}
	uint32_t vw = 0, vh = 0;
	comp_vk_native_renderer_get_view_dimensions(c->renderer, &vw, &vh);
	if (vw == 0 || vh == 0) {
		return false;
	}
	*out_w = vw;
	*out_h = vh;
	return true;
}
