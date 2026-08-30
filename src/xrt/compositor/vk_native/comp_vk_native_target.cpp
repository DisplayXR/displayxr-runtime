// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan presentation target (Win32 surface + VkSwapchainKHR).
 * @author David Fattal
 * @ingroup comp_vk_native
 */

#include "comp_vk_native_target.h"
#include "comp_vk_native_compositor.h"

#include "xrt/xrt_vulkan_includes.h"
#include "vk/vk_helpers.h"

#include "util/u_logging.h"
#include "util/u_debug.h"
#include "util/u_misc.h"
#include "util/comp_late_weave_lookahead.h"

#include "os/os_threading.h"

#ifdef XRT_OS_WINDOWS
// #912 — the #850 saturation governor now also runs the VK bridge pacing
// (present_wait-less drivers, i.e. every Intel iGPU). on_stats never fires
// here (bridge frame statistics are DISJOINT); period_ns is seeded via
// set_display_period and everything else (on_mark saturation EMA, depth
// transitions, occlusion reset) is clock-source-agnostic. File scope for the
// same memset reason as the weave-latency log.
#include "util/comp_weave_latency_win.h"
static late_weave_governor g_lw_gov_vk_bridge;
// Defined later in this file; dcomp_setup consults it at creation so the
// bridge waitable is never built on devices where VK's own present_wait
// will pace (nothing should double-pace on NVIDIA).
static PFN_vkWaitForPresentKHR
target_present_wait_fn(struct comp_vk_native_target *target);
#include <atomic>
// #868 interplay: repaint presents release bridge-waitable tokens nobody
// consumed; the app's next acquire drains the surplus (see the D3D targets).
static std::atomic<uint32_t> g_vk_repaint_presents_since_app{0};
#endif
#include "os/os_time.h"

// Portable (all OSes) — the witness is exactly for paths/platforms the
// Windows-only latency harness cannot measure.
#include "util/comp_vblank_grid.h"
#include "util/comp_frame_witness.h"
static comp_frame_witness g_frame_witness_vk{"vk"};

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>

#ifdef XRT_OS_WINDOWS
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>
// Transparent-background present path: VK -> D3D11 KMT shared textures ->
// DComp + CreateSwapChainForComposition flip-model swapchain -> HWND.
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <dcomp.h>

// Decoupled presentation (#833): with DXR_PRESENT_OPAQUE=1 the bridge
// presents through an HWND flip-model swapchain (ALPHA_MODE_IGNORE, no DComp
// visual) - eligible for Hardware Independent Flip on the app's
// WS_EX_NOREDIRECTIONBITMAP window - while the DP keeps compose-under-bg, so
// the see-through look survives (the baked desktop makes the output opaque
// by construction). A DComp visual costs full Composed: Flip regardless of
// alpha mode, and the VK WSI opaque path needs a redirection surface NRB
// windows lack - both measured dead ends on Intel UHD.
DEBUG_GET_ONCE_BOOL_OPTION(present_opaque, "DXR_PRESENT_OPAQUE", false)

/*
 * #870/#912 — DXR_VK_BRIDGE_PACING: pace the VK live/DComp path on the
 * bridge's DXGI frame-latency waitable. Intel iGPUs expose NO Vulkan
 * presentation-timing extension (no present_wait / present_id /
 * GOOGLE_display_timing — verified per-device with vulkaninfo), so VK
 * late-weave is otherwise permanently dormant on the very adapter that scans
 * out the panel. The transparent path already presents through a DXGI
 * object, which can carry the pacing Vulkan cannot.
 *
 * Semantics (getenv NULL is distinct from "0"):
 *   unset -> GOVERNOR mode (default ON where present_wait is absent): depth
 *            starts at the #850 governor's base and auto-backs-off on
 *            saturation, exactly like the D3D paths.
 *   "0"   -> off (kill switch).
 *   N 1-3 -> pin the depth (governor transitions disabled).
 */
#define DXR_VK_BRIDGE_PACING_GOVERNOR (-1)
static int
dxr_vk_bridge_pacing_mode(void)
{
	static int mode = -2;
	if (mode == -2) {
		const char *e = getenv("DXR_VK_BRIDGE_PACING");
		if (e == NULL || e[0] == '\0') {
			mode = DXR_VK_BRIDGE_PACING_GOVERNOR;
		} else {
			int v = atoi(e);
			if (v < 0) {
				v = 0;
			}
			if (v > 3) {
				v = 3;
			}
			mode = v;
		}
	}
	return mode;
}

static inline DXGI_ALPHA_MODE
dxr_present_alpha_mode(void)
{
	if (debug_get_bool_option_present_opaque()) {
		U_LOG_W("DXR_PRESENT_OPAQUE: composition swapchain uses ALPHA_MODE_IGNORE "
		        "(opaque present, DP compose-under-bg keeps the look, #833)");
		return DXGI_ALPHA_MODE_IGNORE;
	}
	return DXGI_ALPHA_MODE_PREMULTIPLIED;
}

#endif

#ifdef XRT_OS_MACOS
#include <vulkan/vulkan_metal.h>
#endif

#ifdef XRT_OS_ANDROID
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan_android.h>
#include <android/native_window.h>
#include <sys/system_properties.h>
#include "android/android_globals.h"

/*!
 * #1146 secondary — latch a hard acquire failure until the next surface
 * generation, instead of re-attempting it every frame.
 *
 * Between `surfaceDestroyed` on the Java UI thread and the #507 poll running
 * `android_custom_surface_refresh_window()` on the app thread, the render /
 * repaint path keeps acquiring against a BufferQueue that has already been
 * abandoned. `vkAcquireNextImageKHR` returns VK_ERROR_SURFACE_LOST_KHR
 * (-1000000000) and nothing latches, so the loop spins at frame rate and emits
 * hundreds of `dequeueBuffer: BufferQueue has been abandoned` /
 * `Failed to acquire target` lines per second. comp_target_swapchain's #528
 * path already does exactly this ("give up until the next generation instead
 * of spinning a hot recreate-fail loop"); this brings the #507 in-process
 * target in line.
 *
 * Kill switch: `setprop debug.dxr.surface_lost_latch 0` restores the
 * retry-every-frame behaviour. Read once per process.
 */
static bool
dxr_surface_lost_latch_enabled(void)
{
	static int cached = -1;
	if (cached < 0) {
		char prop[PROP_VALUE_MAX] = {0};
		cached = (__system_property_get("debug.dxr.surface_lost_latch", prop) > 0 && prop[0] == '0') ? 0 : 1;
	}
	return cached != 0;
}
#endif

// Desktop Linux (X11/XCB). Android also defines XRT_OS_LINUX but uses
// VK_KHR_android_surface, so the XCB path is gated on "Linux AND NOT Android".
#if defined(XRT_OS_LINUX) && !defined(XRT_OS_ANDROID)
#define XRT_OS_LINUX_DESKTOP
#endif

#ifdef XRT_OS_LINUX_DESKTOP
// X11/XCB present path: vkCreateXcbSurfaceKHR from a connection + window id
// carried in struct comp_vk_native_xcb_handle. (VK_USE_PLATFORM_XCB_KHR is
// defined by CMake via xrt_config_vulkan.h when XRT_HAVE_XCB.)
#include <vulkan/vulkan_xcb.h>
#include <xcb/xcb.h>
#include "comp_vk_native_window_xcb.h"

#endif

#define DCOMP_RING 2 // Number of shared back-buffers in the bridge ring

// Upper bound on swapchain images we track. The driver may create more than
// the minImageCount we request (it's a floor, not a cap) — Adreno commonly
// hands back 5-6 — and vkAcquireNextImageKHR can return ANY index in
// [0, actual_count), so this must be ≥ the largest count any driver returns.
// Was 4, which silently under-captured on Adreno and made get_current_image
// read out of bounds for the surplus images (garbage image/view handles).
#define MAX_TARGET_IMAGES 8

#ifdef XRT_OS_WINDOWS
/*
 * Weave-latency measurement harness (env-gated; VK twin of the D3D11 service
 * harness). DXR_WEAVE_LATENCY_CSV=<prefix> writes <prefix>.vknative.csv with
 * the same row shapes the shared parser understands:
 *   H,qpc_freq
 *   F,seq,qpc_weave,qpc_present_ret,present_id,repaint
 *   S,present_id,0,0,scanout_qpc,qpc_now
 * The trailing F field marks a repaint (re-weave of an unchanged atlas, #868)
 * so the two present populations can be counted apart — same shape as the
 * D3D11/D3D12 sites; dropping it here made every capped-app VK run read as
 * one mixed population (#1048). Older readers ignore the extra column.
 * Scanout truth comes from VK_KHR_present_wait: each present is tagged with a
 * VkPresentIdKHR and a waiter thread timestamps vkWaitForPresentKHR(id)
 * returning — the moment the frame is on glass. Requires the app to have
 * enabled the presentId/presentWait features at device creation (our test
 * apps do when the runtime advertises the extensions); when the proc is
 * missing the harness stays dormant.
 */
/*
 * Late-weave (DXR_LATE_WEAVE=1): pace the frame on the PREVIOUS present
 * hitting glass (vkWaitForPresentKHR in acquire) — the VK equivalent of the
 * D3D11 frame-latency waitable. The whole record+weave+submit+present then
 * runs inside one refresh interval and the FIFO queue stays empty.
 */
static bool
dxr_late_weave_enabled()
{
	static int enabled = -1;
	if (enabled < 0) {
		// Default ON: late-weave is the product behavior on every path
		// (measured 96->17 ms VK, 62->17 D3D12, 32->17 D3D11, 29->17
		// workspace). DXR_LATE_WEAVE=0 opts out for A/B or triage.
		const char *e = getenv("DXR_LATE_WEAVE");
		enabled = (e != nullptr && e[0] == '0') ? 0 : 1;
	}
	return enabled == 1;
}

// #850: DXR_LATE_WEAVE_MAX_LATENCY=N (1..3, default 1). N>1 paces on the
// present N-1 back, restoring N-1 frames of CPU/GPU overlap on a saturated
// pipeline at the cost of that much extra weave-time prediction horizon.
// (The DXGI paths additionally auto-back-off on measured saturation; the VK
// pacer honors the explicit knob only for now.)
static int
vk_late_weave_max_latency()
{
	static int lat = -1;
	if (lat < 0) {
		const char *e = getenv("DXR_LATE_WEAVE_MAX_LATENCY");
		int v = (e != nullptr && e[0] != '\0') ? atoi(e) : 1;
		// Ceiling matches LATE_WEAVE_MAX_DEPTH on the DXGI paths: a 16 ms
		// frame on a 240 Hz panel wants ceil(16/4.17) = 4 frames of queue.
		lat = v < 1 ? 1 : (v > 4 ? 4 : v);
	}
	return lat;
}

struct wl_harness
{
	FILE *f = nullptr;
	uint64_t pending_weave_qpc = 0;
	//! #1048: whether the pending weave is a repaint (re-weave of an unchanged
	//! atlas, #868). Set at the mark alongside pending_weave_qpc, consumed by
	//! the F-row write — the D3D11/D3D12 sites already tag this; dropping it
	//! here made every capped-app VK run read as one mixed population.
	bool pending_repaint = false;
	//! #1044: F rows actually written; teardown WARNs when the harness armed
	//! but produced nothing (e.g. the DComp bridge path, which never presents
	//! through the VK swapchain the waiter watches).
	uint64_t rows_written = 0;
	std::thread waiter;
	std::mutex mtx;
	std::condition_variable cv;
	std::deque<uint64_t> ids;
	bool stop = false;
};

static struct wl_harness *
wl_get(struct comp_vk_native_target *target);
static void
wl_teardown(struct comp_vk_native_target *target);
#endif

/*!
 * Vulkan target structure.
 */
struct comp_vk_native_target
{
	//! Vulkan bundle (borrowed).
	struct vk_bundle *vk;

	//! Win32 surface.
	VkSurfaceKHR surface;

	//! Swapchain.
	VkSwapchainKHR swapchain;

	/*!
	 * Guards the LIFETIME of @ref swapchain (and, in DComp-bridge mode, the
	 * imported ring that stands in for it) against threads that touch it
	 * WITHOUT the compositor lock.
	 *
	 * The compositor lock (`c->mutex`) serialises the frame path against the
	 * #868 repaint replay, but the repaint's PACING deliberately runs
	 * unlocked — @ref comp_vk_native_target_repaint_pace blocks for up to
	 * 100 ms and holding `c->mutex` there would stall the app's frame path
	 * for the same. That left it dereferencing @ref swapchain while the app
	 * thread was in @ref comp_vk_native_target_resize, which nulls the handle
	 * before recreating it: `vkWaitForPresentKHR(device, VK_NULL_HANDLE, …)`
	 * → the ICD takes an SRW lock at `null+0x198` → hard process death. That
	 * is the fullscreen crash; a window resize is the only moment the handle
	 * is ever null.
	 *
	 * A `vkDeviceWaitIdle` does NOT substitute for this: it drains the GPU,
	 * and the race is entirely on the CPU. Vulkan requires the swapchain to
	 * be externally synchronised and this is that synchronisation.
	 *
	 * Held (briefly) by every unlocked swapchain user and (for the whole
	 * destroy+create) by every recreate path.
	 */
	struct os_mutex swapchain_mutex;

	/*!
	 * A recreate wants @ref swapchain_mutex. A HINT ONLY — the mutex is the
	 * correctness mechanism; this just lets a sliced wait bail at its next
	 * slice boundary instead of making the recreate wait out the slice. A
	 * stale read costs one extra slice and nothing else, so a plain bool is
	 * deliberate (this struct is `U_TYPED_CALLOC`'d, so it cannot hold a
	 * `std::atomic` — no constructor ever runs).
	 */
	bool recreate_pending;

	//! Swapchain images.
	VkImage images[MAX_TARGET_IMAGES];

	//! Swapchain image views.
	VkImageView views[MAX_TARGET_IMAGES];

	//! Number of swapchain images.
	uint32_t image_count;

	//! Current acquired image index.
	uint32_t current_index;

	//! Semaphore signaled when image is available.
	VkSemaphore image_available;

	//! Semaphore signaled when rendering is done.
	VkSemaphore render_finished;

	//! Current dimensions.
	uint32_t width;
	uint32_t height;

	//! #602: monotonic counter bumped every time the target image set is
	//! (re)created (window resize → swapchain / DComp-bridge ring rebuild).
	//! The display processor watches this to invalidate any cache it keeps
	//! keyed by the target VkImage handle — Vulkan recycles freed image
	//! handles, so a stale cache entry can otherwise alias a destroyed image
	//! and fault the device on use.
	uint32_t generation;

	//! Surface format.
	VkFormat format;

#ifdef XRT_OS_ANDROID
	//! #1074: last DISPLAY orientation seen (1 landscape / 0 portrait / -1
	//! unknown), from the panel extent published with the window rect. Used
	//! ONLY to tell a device rotation apart from a window resize — never to
	//! decide whether to recreate, which every extent change requires.
	int last_display_landscape;
#endif

	/*!
	 * #868/#902 Android: the vblank grid fed from VK_GOOGLE_display_timing.
	 *
	 * Adreno exposes no VK_KHR_present_wait, so the blocking pacing primitive
	 * below never resolves there and both late weave and the repaint loop fall
	 * back to an open-loop guess against a hardcoded refresh rate — while the
	 * platform switches the panel across [60, 90, 120, 144] underneath them.
	 * This is the retrospective substitute: where presents actually landed,
	 * plus a measured period, projected forward. Untrusted until both halves
	 * exist and are fresh; see comp_vblank_grid.h.
	 */
	struct comp_vblank_grid vblank_grid;
	//! Cached vkGetRefreshCycleDurationGOOGLE result; re-read on recreate.
	bool refresh_cycle_probed;
	//! One-shot check that the driver's present timestamps share our clock.
	bool timebase_checked;
	bool timebase_ok;
	//! Present id for VkPresentTimesInfoGOOGLE tagging. Cross-platform: the
	//! present_id_counter below is inside the Windows block, and this path is
	//! specifically for platforms that have no present_wait.
	uint32_t display_timing_present_id;
	//! When the refresh period was last re-read (ns). The panel is
	//! variable-refresh, so this is polled rather than latched.
	uint64_t refresh_last_probe_ns;

#ifdef XRT_OS_WINDOWS
	//! Weave-latency harness state (nullptr unless DXR_WEAVE_LATENCY_CSV set).
	struct wl_harness *wl;

	//! Present-id state shared by the harness and late-weave pacing.
	//! Ids are per-swapchain; reset on recreate. 0 = present_wait probed
	//! and unavailable; resolved lazily.
	PFN_vkWaitForPresentKHR present_wait_fn;
	bool present_wait_probed;
	uint64_t present_id_counter;
	uint64_t last_present_id;

	//! Weave→scanout measurement for the DP timing feedback loop
	//! (set_frame_timing): the acquire pacer waits for the previous
	//! present to hit glass, so at wait-return "now − that frame's
	//! weave-record QPC" IS the measured residual.
	uint64_t weave_mark_qpc;
	uint64_t last_present_weave_qpc;
	uint64_t measured_r_ns;

	//! #850: per-present weave-record QPC ring (indexed id % 8) so the pacer
	//! can resolve the residual of the present it actually waited on when
	//! DXR_LATE_WEAVE_MAX_LATENCY > 1 targets an older id. Sized well above
	//! the depth ceiling (4) so a slot is never aliased by a newer present
	//! before the pacer reads it.
	uint64_t present_qpc_ring_id[8];
	uint64_t present_qpc_ring[8];

	//! #867: weave-mark to weave-mark EMA — the app's own frame cost, the
	//! other half of the wait_frame->scanout lookahead alongside measured R.
	uint64_t last_mark_qpc;
	double interval_ema_ns;
	double period_hint_ns;
	//! #867: wait_frame-return → weave span (app render + pacer wait).
	uint64_t wait_frame_qpc;
	double wait_to_weave_ema_ns;
#endif

	//! Window handle.
	void *hwnd;

	//! True when @ref surface was created and is owned by an external backend
	//! (the direct-scanout window backend builds the display-plane surface
	//! itself). Teardown then skips vkDestroySurfaceKHR — the backend releases
	//! it, after the display, on its own destroy. ST-5539.
	bool external_surface;

#ifdef XRT_OS_ANDROID
	//! The ANativeWindow the current VkSurfaceKHR was built from (owns one
	//! reference, released when the surface is torn down). Distinct from @ref
	//! hwnd, which is the launch window and is not re-pointed on resume.
	void *android_window;

	//! android_globals surface generation this target's VkSurfaceKHR matches.
	//! A mismatch on the next frame means the SurfaceView handed us a new
	//! surface (resume) — rebuild — or lost it (background) — tear down. #507
	uint64_t surface_generation;

	//! True while there is no live output surface (backgrounded). The compositor
	//! skips acquire/present so the render thread never blocks on a dead window.
	bool surface_lost;
#endif

	//! Queue family index for present support check.
	uint32_t queue_family_index;

	//! True if the swapchain was requested with a transparent compositeAlpha.
	bool transparent_background;

#ifdef XRT_OS_WINDOWS
	// VK -> D3D11 -> DComp transparent present bridge. Active when
	// transparent_background is set AND the bridge initialized successfully.
	// In this mode @ref swapchain stays VK_NULL_HANDLE — VK doesn't go through
	// WSI at all. The compositor renders into @ref dcomp_vk_image[i]; present
	// dispatches to the bridge, which copies to a flip-model DXGI swapchain
	// back buffer that DComp targets the HWND.
	bool dcomp_active;
	ID3D11Device *dcomp_dx_device;
	ID3D11DeviceContext *dcomp_dx_context;
	IDXGISwapChain1 *dcomp_swapchain;
	//! #870 — creation flags, so ResizeBuffers can pass them back (DXGI
	//! E_INVALIDARGs a mismatch; this is the #848 trap, and adding the
	//! waitable flag below is exactly what would have re-armed it).
	UINT dcomp_swapchain_flags;
	//! #870 — requested bridge queue depth (DXR_VK_BRIDGE_PACING), 0 = off.
	int dcomp_pacing_depth;
	//! Drag-shallow (#912): while the present origin is moving (window drag)
	//! the weave phase is anchored to the origin sampled at WEAVE time, but
	//! the frame reaches glass queue-depth ticks later — at governor depth
	//! 2-3 that is a 30-50 ms phase error, visible as 3D stutter during the
	//! drag (repro: avatar RMB-move; the repaint path paces shallow and was
	//! clean). While origin motion is recent, clamp the bridge queue to 1
	//! (banked tokens drained) so phase error returns to ~1 tick; restore
	//! the governor depth when the drag ends.
	uint64_t origin_motion_until_ns;
	bool drag_shallow;
	//! #870 — frame-latency waitable on the BRIDGE swapchain. Intel iGPUs
	//! expose no Vulkan presentation-timing extension at all, so VK
	//! late-weave pacing (VK_KHR_present_wait) is permanently dormant there
	//! — on the adapter that scans out our panels. The transparent path
	//! already presents through this DXGI object, so it can carry the pacing
	//! Vulkan cannot. NULL when late-weave is off or the QI failed.
	HANDLE dcomp_frame_latency_waitable;
	IDCompositionDevice *dcomp_dcomp_device;
	IDCompositionTarget *dcomp_dcomp_target;
	IDCompositionVisual *dcomp_dcomp_visual;
	ID3D11Texture2D *dcomp_shared_dx[DCOMP_RING];
	IDXGIKeyedMutex *dcomp_shared_mutex[DCOMP_RING];
	VkImage dcomp_vk_image[DCOMP_RING];
	VkImageView dcomp_vk_view[DCOMP_RING];
	VkDeviceMemory dcomp_vk_memory[DCOMP_RING];
	uint32_t dcomp_ring_idx;
#endif
};

namespace {
/*!
 * RAII holder for @ref comp_vk_native_target::swapchain_mutex, taken by the
 * paths that DESTROY and rebuild the swapchain / imported ring.
 *
 * Sets `recreate_pending` before blocking on the lock so a sliced waiter
 * (repaint pacing, harness) yields at its next slice instead of making us wait
 * the slice out. RAII rather than bare lock/unlock because the recreate paths
 * are studded with early returns.
 *
 * NOT recursive: never construct one while another is live on this thread.
 * `wl_teardown` joins a thread that takes this mutex, so it must be called
 * BEFORE the guard is constructed, never inside its scope.
 */
struct target_recreate_guard
{
	struct comp_vk_native_target *t;
	explicit target_recreate_guard(struct comp_vk_native_target *t_) : t(t_)
	{
		t->recreate_pending = true;
		os_mutex_lock(&t->swapchain_mutex);
	}
	~target_recreate_guard()
	{
		t->recreate_pending = false;
		os_mutex_unlock(&t->swapchain_mutex);
	}
	target_recreate_guard(const target_recreate_guard &) = delete;
	target_recreate_guard &operator=(const target_recreate_guard &) = delete;
};
} // namespace

static void
destroy_swapchain_views(struct comp_vk_native_target *target)
{
	struct vk_bundle *vk = target->vk;
	for (uint32_t i = 0; i < target->image_count; i++) {
		if (target->views[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, target->views[i], NULL);
			target->views[i] = VK_NULL_HANDLE;
		}
	}
}

static xrt_result_t
create_swapchain_views(struct comp_vk_native_target *target)
{
	struct vk_bundle *vk = target->vk;
	for (uint32_t i = 0; i < target->image_count; i++) {
		VkImageViewCreateInfo ci = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image = target->images[i],
		    .viewType = VK_IMAGE_VIEW_TYPE_2D,
		    .format = target->format,
		    .subresourceRange = {
		        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel = 0,
		        .levelCount = 1,
		        .baseArrayLayer = 0,
		        .layerCount = 1,
		    },
		};

		VkResult res = vk->vkCreateImageView(vk->device, &ci, NULL, &target->views[i]);
		if (res != VK_SUCCESS) {
			U_LOG_E("Failed to create target image view %u: %d", i, res);
			return XRT_ERROR_VULKAN;
		}
	}
	return XRT_SUCCESS;
}

static xrt_result_t
create_swapchain(struct comp_vk_native_target *target)
{
	struct vk_bundle *vk = target->vk;

	// Query surface capabilities
	VkSurfaceCapabilitiesKHR caps;
	VkResult res = vk->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
	    vk->physical_device, target->surface, &caps);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to get surface capabilities: %d", res);
		return XRT_ERROR_VULKAN;
	}

	// Use requested dimensions or surface extent
	VkExtent2D extent = {target->width, target->height};
	if (caps.currentExtent.width != UINT32_MAX) {
		extent = caps.currentExtent;
	}
	target->width = extent.width;
	target->height = extent.height;

	uint32_t image_count = caps.minImageCount + 1;
#ifdef XRT_OS_WINDOWS
	// Late-weave: request the floor. The +1 exists for acquire throughput,
	// but every extra image is another refresh of FIFO present-queue depth —
	// measured 95.8 ms weave->scanout on this path with the default chain.
	// With present_wait pacing (below) the queue never fills anyway.
	// At depth N > 1 (#850) the pacer deliberately keeps N-1 frames in
	// flight, so the chain needs N+1 images or acquire becomes the
	// bottleneck instead of the pacer. At depth 1 this reduces to the floor
	// (minImageCount is 2 on every driver we ship against).
	if (dxr_late_weave_enabled()) {
		const uint32_t want = (uint32_t)vk_late_weave_max_latency() + 1;
		image_count = (caps.minImageCount > want) ? caps.minImageCount : want;
	}
#endif
	if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
		image_count = caps.maxImageCount;
	}
	if (image_count > MAX_TARGET_IMAGES) {
		image_count = MAX_TARGET_IMAGES;
	}

	// Pick surface format
	uint32_t format_count = 0;
	vk->vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physical_device, target->surface,
	                                          &format_count, NULL);
	VkSurfaceFormatKHR formats[32];
	if (format_count > 32) format_count = 32;
	vk->vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physical_device, target->surface,
	                                          &format_count, formats);

	// Prefer BGRA8_UNORM, fall back to first available
	target->format = formats[0].format;
	VkColorSpaceKHR color_space = formats[0].colorSpace;
	for (uint32_t i = 0; i < format_count; i++) {
		if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
			target->format = formats[i].format;
			color_space = formats[i].colorSpace;
			break;
		}
	}

	// Pick present mode: FIFO (VSync) is always available
	VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;

	// Pick the surface pre-transform. Default: match the surface's current
	// transform so the WSI rotates our output for us (correct for normal 2D/3D
	// content).
	VkSurfaceTransformFlagBitsKHR pre_transform = caps.currentTransform;
#ifdef XRT_OS_ANDROID
	// LOXR-730/733 (landscape weave ghost): a Leia-WOVEN image cannot be
	// rotated by the WSI — the interlaced sub-pixel pattern must stay aligned to
	// the physically-bonded, panel-native lenticular. With Android pre-rotation,
	// in landscape the surface is panel-native portrait (currentExtent
	// 1600x2560) + currentTransform=ROTATE_90, so presenting our woven buffer
	// with preTransform=ROTATE_90 shears the weave off the lenticular (ghosting;
	// portrait is clean because currentTransform is IDENTITY there). Force
	// IDENTITY so the woven buffer scans out 1:1 onto the panel; content is
	// rotated per-orientation upstream instead of rotating the woven image.
	if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
		pre_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	}
	U_LOG_W("HW_XFORM: currentTransform=0x%x supported=0x%x chosen_preTransform=0x%x extent=%ux%u",
	        (unsigned)caps.currentTransform, (unsigned)caps.supportedTransforms,
	        (unsigned)pre_transform, extent.width, extent.height);
#endif

	// Pick compositeAlpha. The DP's chroma-key strip pass writes premultiplied
	// alpha into the swapchain image, so we want PRE_MULTIPLIED. INHERIT works
	// on some Win32 WSI drivers where DWM still respects the alpha channel.
	// Most Win32 ICDs only expose OPAQUE — in that case transparency silently
	// no-ops at the WSI layer (the strip pass still runs but the alpha is
	// dropped on present); we log a one-time warning so the failure mode is
	// visible without spamming per-frame.
	VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if (target->transparent_background) {
		U_LOG_I("VK target: transparent_background requested, supportedCompositeAlpha=0x%x",
		        (unsigned)caps.supportedCompositeAlpha);
		if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) {
			composite_alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
			U_LOG_I("VK target: transparent_background using PRE_MULTIPLIED");
#if defined(XRT_OS_MACOS)
		} else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) {
			// MoltenVK does not expose PRE_MULTIPLIED, but it honors
			// POST_MULTIPLIED by forcing CAMetalLayer.opaque = NO. It does
			// not premultiply/un-premultiply — Core Animation always
			// composites premultiplied bytes, and our content is already
			// premultiplied (renderer emits alpha = 1 - T premultiplied), so
			// the present is correct. This is more reliable than INHERIT,
			// which depends on the app's CAMetalLayer.opaque state surviving
			// AppKit's layer-backed-view sync. macOS-only on purpose: on
			// Win32 ICDs POST_MULTIPLIED would mean straight alpha and break
			// the (premultiplied) content — Windows uses the DComp bridge or
			// PRE_MULTIPLIED instead.
			composite_alpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
			U_LOG_I("VK target: transparent_background using POST_MULTIPLIED (macOS/MoltenVK)");
#endif
		} else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
			composite_alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
			U_LOG_I("VK target: transparent_background using INHERIT (PRE_MULTIPLIED unavailable)");
		} else {
			U_LOG_W("VK target: transparent_background requested but no transparent "
			        "compositeAlpha is supported (caps=0x%x); falling back to OPAQUE — "
			        "alpha will be dropped at WSI present",
			        (unsigned)caps.supportedCompositeAlpha);
		}
	}

	VkSwapchainCreateInfoKHR ci = {
	    .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
	    .surface = target->surface,
	    .minImageCount = image_count,
	    .imageFormat = target->format,
	    .imageColorSpace = color_space,
	    .imageExtent = extent,
	    .imageArrayLayers = 1,
	    .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	    .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .preTransform = pre_transform,
	    .compositeAlpha = composite_alpha,
	    .presentMode = present_mode,
	    .clipped = VK_TRUE,
	    .oldSwapchain = VK_NULL_HANDLE,
	};

	res = vk->vkCreateSwapchainKHR(vk->device, &ci, NULL, &target->swapchain);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create swapchain: %d", res);
		return XRT_ERROR_VULKAN;
	}

	// Get swapchain images. Query the real count first — the driver may
	// create more images than the minImageCount we requested (Adreno does),
	// and acquire can return any index in [0, count), so we must capture all
	// of them. Under-capturing makes get_current_image read past images[]/
	// views[] and hand the DP a garbage handle.
	uint32_t actual_count = 0;
	res = vk->vkGetSwapchainImagesKHR(vk->device, target->swapchain, &actual_count, NULL);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to query swapchain image count: %d", res);
		return XRT_ERROR_VULKAN;
	}
	if (actual_count > MAX_TARGET_IMAGES) {
		U_LOG_E("Swapchain has %u images, exceeds MAX_TARGET_IMAGES=%u — raise the cap",
		        actual_count, (uint32_t)MAX_TARGET_IMAGES);
		return XRT_ERROR_VULKAN;
	}
	target->image_count = actual_count;
	res = vk->vkGetSwapchainImagesKHR(vk->device, target->swapchain,
	                                    &target->image_count, target->images);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to get swapchain images: %d", res);
		return XRT_ERROR_VULKAN;
	}

	// Create image views
	xrt_result_t vres = create_swapchain_views(target);
	if (vres == XRT_SUCCESS) {
		// #602: a new image set. Bumped HERE, not at the call sites, so every
		// rebuild -- resize, the Android surface re-sync (#507), the
		// acquire-side VK_ERROR_OUT_OF_DATE_KHR retry -- reaches the DP through
		// vk_dp_weave_and_present's generation check (notify_target_recreated).
		// The re-sync and acquire paths never bumped it before, and a DP that
		// keys caches on our VkImageView handles (the driver recycles them) then
		// hit stale entries: one swapchain slot weaving into a dead image after
		// every Android background/resume cycle.
		target->generation++;
	}
	return vres;
}

#ifdef XRT_OS_WINDOWS

/*
 *
 * VK -> D3D11 -> DComp transparent present bridge.
 *
 * Win32 Vulkan WSI doesn't expose any path to alpha-correct desktop
 * composition (most ICDs only advertise OPAQUE compositeAlpha). To get real
 * desktop see-through under VK we side-step WSI entirely:
 *   1. Create a D3D11 device (anyone — DXGI factory + DComp don't care which
 *      adapter the VK GPU is on, we'll Copy across them via the system bus).
 *   2. For each ring slot create an ID3D11Texture2D with KMT_BIT shared NT
 *      handle + KEYEDMUTEX. Open the KMT handle in VK via
 *      VK_KHR_external_memory_win32 (which the runtime already requires).
 *   3. Create a flip-model DXGI swapchain via CreateSwapChainForComposition
 *      with PRE_MULTIPLIED alpha. Bind to the HWND through DComp visual+target.
 *   4. Each frame: VK renders into ring[i]'s VkImage. After vkQueueWaitIdle,
 *      D3D11 IDXGIKeyedMutex::AcquireSync(0,0) flushes the writer caches
 *      (per memory feedback_acquiresync_load_bearing.md), CopyResource into
 *      the swapchain back buffer, ReleaseSync, swapchain->Present(1, 0),
 *      dcomp_device->Commit().
 *
 * Ring index is bumped by acquire(); compositor renders into the slot
 * returned by get_current_image(); present() copies that slot.
 *
 */

static void
dcomp_destroy(struct comp_vk_native_target *target)
{
	if (target == NULL) return;
	struct vk_bundle *vk = target->vk;

	for (uint32_t i = 0; i < DCOMP_RING; i++) {
		if (target->dcomp_vk_view[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, target->dcomp_vk_view[i], NULL);
			target->dcomp_vk_view[i] = VK_NULL_HANDLE;
		}
		if (target->dcomp_vk_image[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, target->dcomp_vk_image[i], NULL);
			target->dcomp_vk_image[i] = VK_NULL_HANDLE;
		}
		if (target->dcomp_vk_memory[i] != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, target->dcomp_vk_memory[i], NULL);
			target->dcomp_vk_memory[i] = VK_NULL_HANDLE;
		}
		if (target->dcomp_shared_mutex[i]) {
			target->dcomp_shared_mutex[i]->Release();
			target->dcomp_shared_mutex[i] = NULL;
		}
		if (target->dcomp_shared_dx[i]) {
			target->dcomp_shared_dx[i]->Release();
			target->dcomp_shared_dx[i] = NULL;
		}
	}
	if (target->dcomp_dcomp_visual) { target->dcomp_dcomp_visual->Release(); target->dcomp_dcomp_visual = NULL; }
	if (target->dcomp_dcomp_target) { target->dcomp_dcomp_target->Release(); target->dcomp_dcomp_target = NULL; }
	if (target->dcomp_dcomp_device) { target->dcomp_dcomp_device->Release(); target->dcomp_dcomp_device = NULL; }
	if (target->dcomp_swapchain)    { target->dcomp_swapchain->Release();    target->dcomp_swapchain = NULL; }
	if (target->dcomp_dx_context)   { target->dcomp_dx_context->Release();   target->dcomp_dx_context = NULL; }
	if (target->dcomp_dx_device)    { target->dcomp_dx_device->Release();    target->dcomp_dx_device = NULL; }
	target->dcomp_active = false;
}

// Import a single D3D11 NT-handle-shared texture as a VkImage in the ring.
static bool
dcomp_import_one(struct comp_vk_native_target *target,
                 uint32_t i,
                 ID3D11Texture2D *dx_tex,
                 HANDLE shared_nt,
                 uint32_t w,
                 uint32_t h,
                 VkFormat vk_format)
{
	struct vk_bundle *vk = target->vk;

	VkExternalMemoryImageCreateInfo external_ci = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	    .pNext = NULL,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
	};

	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &external_ci,
	    .flags = 0,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = vk_format,
	    .extent = {w, h, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	             VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkResult res = vk->vkCreateImage(vk->device, &image_ci, NULL, &target->dcomp_vk_image[i]);
	if (res != VK_SUCCESS) {
		U_LOG_E("DComp bridge: vkCreateImage failed for ring[%u]: %d", i, res);
		return false;
	}

	VkMemoryRequirements requirements = {};
	vk->vkGetImageMemoryRequirements(vk->device, target->dcomp_vk_image[i], &requirements);

	VkImportMemoryWin32HandleInfoKHR import_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
	    .pNext = NULL,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
	    .handle = shared_nt,
	};
	VkMemoryDedicatedAllocateInfoKHR dedicated_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
	    .pNext = &import_info,
	    .image = target->dcomp_vk_image[i],
	    .buffer = VK_NULL_HANDLE,
	};

	VkPhysicalDeviceMemoryProperties mem_props = {};
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);
	uint32_t memory_type_index = UINT32_MAX;
	for (uint32_t k = 0; k < mem_props.memoryTypeCount; k++) {
		if ((requirements.memoryTypeBits & (1u << k)) != 0) {
			memory_type_index = k;
			break;
		}
	}
	if (memory_type_index == UINT32_MAX) {
		U_LOG_E("DComp bridge: no compatible memory type for ring[%u]", i);
		return false;
	}

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &dedicated_info,
	    .allocationSize = requirements.size,
	    .memoryTypeIndex = memory_type_index,
	};
	res = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &target->dcomp_vk_memory[i]);
	if (res != VK_SUCCESS) {
		U_LOG_E("DComp bridge: vkAllocateMemory failed for ring[%u]: %d", i, res);
		return false;
	}

	res = vk->vkBindImageMemory(vk->device, target->dcomp_vk_image[i],
	                            target->dcomp_vk_memory[i], 0);
	if (res != VK_SUCCESS) {
		U_LOG_E("DComp bridge: vkBindImageMemory failed for ring[%u]: %d", i, res);
		return false;
	}

	VkImageViewCreateInfo view_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = target->dcomp_vk_image[i],
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = vk_format,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	res = vk->vkCreateImageView(vk->device, &view_ci, NULL, &target->dcomp_vk_view[i]);
	if (res != VK_SUCCESS) {
		U_LOG_E("DComp bridge: vkCreateImageView failed for ring[%u]: %d", i, res);
		return false;
	}

	// Cache the IDXGIKeyedMutex for the cross-API sync.
	HRESULT hr = dx_tex->QueryInterface(__uuidof(IDXGIKeyedMutex),
	                                     (void **)&target->dcomp_shared_mutex[i]);
	if (FAILED(hr) || target->dcomp_shared_mutex[i] == NULL) {
		U_LOG_E("DComp bridge: QueryInterface(IDXGIKeyedMutex) failed for ring[%u]: 0x%08x", i, hr);
		return false;
	}

	return true;
}

// Initialize the DComp bridge: D3D11 device, swapchain, DComp visual+target,
// and the ring of KMT-shared textures imported as VkImages. Returns false
// (with a U_LOG_W) if any prerequisite is missing — caller falls back to
// opaque WSI.
// Packed LUID (HighPart<<32 | LowPart) of the VkPhysicalDevice, 0 if unknown.
static uint64_t
vk_device_packed_luid(struct vk_bundle *vk)
{
	if (vk->vkGetPhysicalDeviceProperties2 == NULL) {
		return 0;
	}
	VkPhysicalDeviceIDProperties id_props = {};
	id_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
	VkPhysicalDeviceProperties2 props2 = {};
	props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	props2.pNext = &id_props;
	vk->vkGetPhysicalDeviceProperties2(vk->physical_device, &props2);
	if (!id_props.deviceLUIDValid) {
		return 0;
	}
	uint64_t luid = 0;
	memcpy(&luid, id_props.deviceLUID, sizeof(luid));
	return luid;
}

static bool
dcomp_setup(struct comp_vk_native_target *target, HWND hwnd, uint32_t w, uint32_t h)
{
	struct vk_bundle *vk = target->vk;

	// The bridge's D3D11 device MUST live on the same adapter as the
	// VkDevice: the ring textures are KMT-shared between the two, and a
	// shared surface does not carry pixels across adapters — on an Optimus
	// box with the VkDevice forced to the iGPU, a default-adapter bridge
	// device silently presents a never-written (fully transparent) surface.
	// Match by LUID; fall back to opaque WSI rather than to a mismatched
	// default device.
	const uint64_t want_luid = vk_device_packed_luid(vk);
	IDXGIAdapter1 *create_adapter = NULL;
	if (want_luid != 0) {
		IDXGIFactory1 *enum_factory = NULL;
		if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&enum_factory))) {
			for (UINT i = 0; enum_factory->EnumAdapters1(i, &create_adapter) != DXGI_ERROR_NOT_FOUND;
			     i++) {
				DXGI_ADAPTER_DESC1 ad = {};
				uint64_t luid = 0;
				if (SUCCEEDED(create_adapter->GetDesc1(&ad))) {
					luid = ((uint64_t)(uint32_t)ad.AdapterLuid.HighPart << 32) |
					       (uint64_t)(uint32_t)ad.AdapterLuid.LowPart;
				}
				if (luid == want_luid) {
					U_LOG_W("DComp bridge: matching VkDevice adapter LUID 0x%016llx (%ls)",
					        (unsigned long long)luid, ad.Description);
					break;
				}
				create_adapter->Release();
				create_adapter = NULL;
			}
			enum_factory->Release();
		}
		if (create_adapter == NULL) {
			U_LOG_W("DComp bridge: no DXGI adapter matches VkDevice LUID 0x%016llx — "
			        "falling back to opaque WSI",
			        (unsigned long long)want_luid);
			return false;
		}
	} else {
		U_LOG_W("DComp bridge: VkDevice LUID unavailable — using default D3D11 adapter");
	}

	HRESULT hr = D3D11CreateDevice(
	    create_adapter, create_adapter != NULL ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL,
	    D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION,
	    &target->dcomp_dx_device, NULL, &target->dcomp_dx_context);
	if (create_adapter != NULL) {
		create_adapter->Release();
	}
	if (FAILED(hr) || target->dcomp_dx_device == NULL) {
		U_LOG_W("DComp bridge: D3D11CreateDevice failed: 0x%08x — falling back to opaque WSI", hr);
		return false;
	}

	// Create flip-model swapchain via DXGI factory bound to the D3D11 device.
	IDXGIDevice *dxgi_device = NULL;
	hr = target->dcomp_dx_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_device);
	if (FAILED(hr) || dxgi_device == NULL) {
		U_LOG_W("DComp bridge: QueryInterface(IDXGIDevice) failed: 0x%08x", hr);
		return false;
	}
	IDXGIAdapter *dxgi_adapter = NULL;
	dxgi_device->GetAdapter(&dxgi_adapter);
	dxgi_device->Release();
	if (dxgi_adapter == NULL) {
		U_LOG_W("DComp bridge: GetAdapter failed");
		return false;
	}
	IDXGIFactory2 *dxgi_factory = NULL;
	hr = dxgi_adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&dxgi_factory);
	dxgi_adapter->Release();
	if (FAILED(hr) || dxgi_factory == NULL) {
		U_LOG_W("DComp bridge: GetParent(IDXGIFactory2) failed: 0x%08x", hr);
		return false;
	}

	// Decoupled presentation (#833): with DXR_PRESENT_OPAQUE=1, present the
	// bridge through an HWND flip-model swapchain instead of DComp. The app's
	// WS_EX_NOREDIRECTIONBITMAP window takes flip-model presents directly and
	// is eligible for Hardware Independent Flip; a DComp visual stays at full
	// Composed: Flip cost regardless of alpha mode (measured), and the VK WSI
	// opaque path presents via a GDI/redirection mechanism NRB windows lack.
	// The transparent look survives via the DP's compose-under-bg.
	const bool present_opaque = debug_get_bool_option_present_opaque();

	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = w;
	desc.Height = h;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = DCOMP_RING;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.AlphaMode = dxr_present_alpha_mode(); // #833: IGNORE when present_opaque
	// #870/#912 — make the bridge swapchain waitable so the VK path can be
	// paced on drivers with no VK_KHR_present_wait (every Intel iGPU).
	// Governor-managed default-ON there (the #850 saturation auto-backoff
	// protects throughput — the reason the earlier opt-in shipped default-off
	// no longer holds); never created when VK's own present_wait exists
	// (nothing should double-pace on NVIDIA) and never when late-weave is
	// globally off.
	const int pacing_mode = dxr_vk_bridge_pacing_mode();
	target->dcomp_pacing_depth = 0;
	if (dxr_late_weave_enabled() && pacing_mode != 0 && target_present_wait_fn(target) == NULL) {
		target->dcomp_pacing_depth =
		    (pacing_mode == DXR_VK_BRIDGE_PACING_GOVERNOR) ? g_lw_gov_vk_bridge.base_latency()
		                                                   : pacing_mode;
		desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
	}
	target->dcomp_swapchain_flags = desc.Flags;

	if (present_opaque) {
		DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs_desc = {};
		fs_desc.Windowed = TRUE;
		hr = dxgi_factory->CreateSwapChainForHwnd(target->dcomp_dx_device, hwnd, &desc, &fs_desc,
		                                           NULL, &target->dcomp_swapchain);
		if (SUCCEEDED(hr)) {
			dxgi_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
			U_LOG_W("DXR_PRESENT_OPAQUE: bridge presents via HWND flip-model swapchain "
			        "(no DComp visual, #833)");
		}
	} else {
		hr = dxgi_factory->CreateSwapChainForComposition(target->dcomp_dx_device, &desc, NULL,
		                                                  &target->dcomp_swapchain);
	}
	dxgi_factory->Release();
	if (FAILED(hr) || target->dcomp_swapchain == NULL) {
		U_LOG_W("DComp bridge: swapchain create failed (present_opaque=%d): 0x%08x", (int)present_opaque,
		        hr);
		return false;
	}

	// #870 — pick up the frame-latency waitable. This is the pacing signal for
	// drivers that expose no Vulkan presentation-timing extension (all Intel
	// iGPUs), where VK late-weave would otherwise stay dormant on exactly the
	// adapter that scans out the panel. Best-effort: a failure here just leaves
	// the path unpaced, which is today's behaviour.
	if ((target->dcomp_swapchain_flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) {
		IDXGISwapChain2 *sc2 = NULL;
		if (SUCCEEDED(target->dcomp_swapchain->QueryInterface(__uuidof(IDXGISwapChain2),
		                                                       (void **)&sc2)) &&
		    sc2 != NULL) {
			sc2->SetMaximumFrameLatency((UINT)target->dcomp_pacing_depth);
			target->dcomp_frame_latency_waitable = sc2->GetFrameLatencyWaitableObject();
			sc2->Release();
		}
		U_LOG_W("#870: bridge frame-latency waitable %s (waitable=%p)",
		        target->dcomp_frame_latency_waitable != NULL ? "acquired" : "UNAVAILABLE",
		        target->dcomp_frame_latency_waitable);
	}

	// DComp binding only on the transparent-present path — the opaque path's
	// swapchain is HWND-bound already (#833).
	if (!present_opaque) {
		hr = DCompositionCreateDevice2(NULL, __uuidof(IDCompositionDevice),
		                                (void **)&target->dcomp_dcomp_device);
		if (FAILED(hr) || target->dcomp_dcomp_device == NULL) {
			U_LOG_W("DComp bridge: DCompositionCreateDevice2 failed: 0x%08x", hr);
			return false;
		}
		hr = target->dcomp_dcomp_device->CreateTargetForHwnd(hwnd, /*topmost*/ TRUE,
		                                                      &target->dcomp_dcomp_target);
		if (FAILED(hr) || target->dcomp_dcomp_target == NULL) {
			U_LOG_W("DComp bridge: CreateTargetForHwnd failed: 0x%08x", hr);
			return false;
		}
		hr = target->dcomp_dcomp_device->CreateVisual(&target->dcomp_dcomp_visual);
		if (FAILED(hr) || target->dcomp_dcomp_visual == NULL) {
			U_LOG_W("DComp bridge: CreateVisual failed: 0x%08x", hr);
			return false;
		}
		if (FAILED(target->dcomp_dcomp_visual->SetContent(target->dcomp_swapchain)) ||
		    FAILED(target->dcomp_dcomp_target->SetRoot(target->dcomp_dcomp_visual)) ||
		    FAILED(target->dcomp_dcomp_device->Commit())) {
			U_LOG_W("DComp bridge: visual setup failed");
			return false;
		}
	}

	// Create the ring of KMT-shared D3D11 textures and import each as a VkImage.
	for (uint32_t i = 0; i < DCOMP_RING; i++) {
		D3D11_TEXTURE2D_DESC tdesc = {};
		tdesc.Width = w;
		tdesc.Height = h;
		tdesc.MipLevels = 1;
		tdesc.ArraySize = 1;
		tdesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		tdesc.SampleDesc.Count = 1;
		tdesc.Usage = D3D11_USAGE_DEFAULT;
		tdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		// NT-handle sharing (CreateSharedHandle + VK import via the NT
		// D3D11_TEXTURE bit). The legacy KMT path silently yields a
		// never-updated — hence fully transparent — surface on Intel UHD
		// 30.0.100.x, while the NT path is the one proven to share pixels
		// with that driver (it is what the WGC bg-capture import uses).
		// The keyed mutex is kept: the reader's AcquireSync(0,0) is the
		// cross-API cache barrier (see present()).
		tdesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

		hr = target->dcomp_dx_device->CreateTexture2D(&tdesc, NULL, &target->dcomp_shared_dx[i]);
		if (FAILED(hr)) {
			U_LOG_W("DComp bridge: CreateTexture2D[%u] failed: 0x%08x", i, hr);
			return false;
		}

		// NT shared handle for the VK import. Ownership stays here — Vulkan
		// does not adopt NT handles, so it is closed right after the import.
		IDXGIResource1 *dxgi_res = NULL;
		hr = target->dcomp_shared_dx[i]->QueryInterface(__uuidof(IDXGIResource1),
		                                                 (void **)&dxgi_res);
		if (FAILED(hr) || dxgi_res == NULL) {
			U_LOG_W("DComp bridge: QueryInterface(IDXGIResource1)[%u] failed: 0x%08x", i, hr);
			return false;
		}
		HANDLE shared_nt = NULL;
		hr = dxgi_res->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
		                                  NULL, &shared_nt);
		dxgi_res->Release();
		if (FAILED(hr) || shared_nt == NULL) {
			U_LOG_W("DComp bridge: CreateSharedHandle[%u] failed: 0x%08x", i, hr);
			return false;
		}

		bool imported = dcomp_import_one(target, i, target->dcomp_shared_dx[i], shared_nt,
		                                  w, h, VK_FORMAT_B8G8R8A8_UNORM);
		CloseHandle(shared_nt);
		if (!imported) {
			return false;
		}
	}

	// Match the public target fields so the rest of the compositor sees the
	// imported VkImages as if they were swapchain images.
	target->image_count = DCOMP_RING;
	for (uint32_t i = 0; i < DCOMP_RING; i++) {
		target->images[i] = target->dcomp_vk_image[i];
		target->views[i] = target->dcomp_vk_view[i];
	}
	target->format = VK_FORMAT_B8G8R8A8_UNORM;
	target->width = w;
	target->height = h;
	target->dcomp_ring_idx = 0;
	target->current_index = 0;
	target->dcomp_active = true;

	U_LOG_W("DComp bridge active: %ux%u, %u-deep ring, NT shared, "
	        "PRE_MULTIPLIED + DComp -> HWND",
	        w, h, (unsigned)DCOMP_RING);
	return true;
}

// Submit the just-rendered ring slot to DComp: D3D11 acquires sync on the
// shared texture (which flushes VK writer caches per
// feedback_acquiresync_load_bearing.md), CopyResource into the next swapchain
// back buffer, releases sync, Present, Commit.
static xrt_result_t
dcomp_present(struct comp_vk_native_target *target)
{
	uint32_t idx = target->current_index;
	if (idx >= DCOMP_RING || target->dcomp_shared_mutex[idx] == NULL) {
		U_LOG_E("DComp bridge: present called with invalid ring index %u", idx);
		return XRT_ERROR_VULKAN;
	}

	// Acquire (key=0) — also flushes VK writer caches into the shared resource.
	HRESULT hr = target->dcomp_shared_mutex[idx]->AcquireSync(0, INFINITE);
	if (FAILED(hr)) {
		U_LOG_E("DComp bridge: AcquireSync[%u] failed: 0x%08x", idx, hr);
		return XRT_ERROR_VULKAN;
	}

	ID3D11Texture2D *back = NULL;
	hr = target->dcomp_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
	if (FAILED(hr) || back == NULL) {
		U_LOG_E("DComp bridge: GetBuffer failed: 0x%08x", hr);
		target->dcomp_shared_mutex[idx]->ReleaseSync(0);
		return XRT_ERROR_VULKAN;
	}

	target->dcomp_dx_context->CopyResource(back, target->dcomp_shared_dx[idx]);
	back->Release();

	target->dcomp_shared_mutex[idx]->ReleaseSync(0);

	hr = target->dcomp_swapchain->Present(/*SyncInterval*/ 1, /*Flags*/ 0);

	// #870 — probe once whether this swapchain can report scanout truth. On
	// Intel there is no VK presentation-timing extension, so the weave-latency
	// harness has no R number on the very adapter that drives the panel: we
	// cannot currently measure how bad the unpaced case is. DXGI frame
	// statistics on a COMPOSITION swapchain are not guaranteed (DWM may report
	// DXGI_ERROR_FRAME_STATISTICS_DISJOINT or nothing at all), so state
	// plainly which it is rather than assuming — this decides whether #870's
	// measurement half is possible here or has to fall back to estimation.
	{
		static bool stats_probed = false;
		if (!stats_probed && SUCCEEDED(hr)) {
			stats_probed = true;
			DXGI_FRAME_STATISTICS fs = {};
			HRESULT shr = target->dcomp_swapchain->GetFrameStatistics(&fs);
			if (SUCCEEDED(shr)) {
				U_LOG_W("#870: bridge frame statistics USABLE "
				        "(PresentCount=%u SyncQPCTime=%lld) — scanout truth available "
				        "without VK_KHR_present_wait",
				        fs.PresentCount, (long long)fs.SyncQPCTime.QuadPart);
			} else {
				U_LOG_W("#870: bridge frame statistics unavailable (0x%08x) — measurement "
				        "half needs estimation, pacing is unaffected",
				        shr);
			}
		}
	}
	if (FAILED(hr)) {
		U_LOG_E("DComp bridge: Present failed: 0x%08x", hr);
		return XRT_ERROR_VULKAN;
	}
	if (target->dcomp_dcomp_device != NULL) { target->dcomp_dcomp_device->Commit(); }
	return XRT_SUCCESS;
}

#endif // XRT_OS_WINDOWS


xrt_result_t
comp_vk_native_target_create(struct comp_vk_native_compositor *c,
                              void *hwnd,
                              bool is_wayland,
                              uint32_t width,
                              uint32_t height,
                              bool transparent_background,
                              struct comp_vk_native_target **out_target)
{
	(void)is_wayland; // used only in the XRT_HAVE_WAYLAND Linux branch below
	struct vk_bundle *vk = comp_vk_native_compositor_get_vk(c);
	uint32_t queue_family_index = comp_vk_native_compositor_get_queue_family(c);

	struct comp_vk_native_target *target = U_TYPED_CALLOC(struct comp_vk_native_target);
	if (target == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	target->vk = vk;
	target->hwnd = hwnd;
	target->width = width;
	target->height = height;
#ifdef XRT_OS_ANDROID
	target->last_display_landscape = -1; // unknown until a rect is published (#1074)
#endif
	target->queue_family_index = queue_family_index;
	target->transparent_background = transparent_background;
	os_mutex_init(&target->swapchain_mutex);

#ifdef XRT_OS_ANDROID
	// Track the surface we build from + the generation it matches, so the
	// per-frame re-sync can detect surface loss / replacement on resume. #507
	//
	// #1040: hold our OWN reference on it. `hwnd` is the window android_globals
	// published (and still owns); adopting that single reference made a second
	// target in the same process — an xrEndSession→xrBeginSession cycle —
	// release a reference nobody had taken, destroying the native Surface under
	// the live Java Surface (SIGSEGV in Surface.finalize).
	target->android_window = hwnd;
	if (target->android_window != NULL) {
		ANativeWindow_acquire((ANativeWindow *)target->android_window);
	}
	target->surface_lost = false;
	android_globals_get_window_state(NULL, &target->surface_generation, NULL);
#endif

#ifdef XRT_OS_WINDOWS
	// Transparent-background path: VK -> D3D11 KMT shared -> DComp bridge,
	// no WSI swapchain at all. Falls back to opaque WSI on failure.
	if (transparent_background && hwnd != NULL) {
		if (dcomp_setup(target, (HWND)hwnd, width, height)) {
			*out_target = target;
			return XRT_SUCCESS;
		}
		// Setup failed: tear down anything dcomp_setup partially created and
		// fall through to the standard WSI path below (will end up OPAQUE).
		dcomp_destroy(target);
	}

	// Create Win32 surface
	// Note: vkCreateWin32SurfaceKHR is an instance-level function loaded
	// into vk_bundle by vk_get_instance_functions(). Access via vk->vkCreateWin32SurfaceKHR.
	PFN_vkCreateWin32SurfaceKHR pvkCreateWin32SurfaceKHR =
	    (PFN_vkCreateWin32SurfaceKHR)vk->vkGetInstanceProcAddr(vk->instance, "vkCreateWin32SurfaceKHR");
	if (pvkCreateWin32SurfaceKHR == NULL) {
		U_LOG_E("Failed to load vkCreateWin32SurfaceKHR");
		free(target);
		return XRT_ERROR_VULKAN;
	}

	VkWin32SurfaceCreateInfoKHR surface_ci = {
	    .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
	    .hinstance = GetModuleHandle(NULL),
	    .hwnd = (HWND)hwnd,
	};

	VkResult res = pvkCreateWin32SurfaceKHR(vk->instance, &surface_ci, NULL, &target->surface);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create Win32 surface: %d", res);
		free(target);
		return XRT_ERROR_VULKAN;
	}

	// Check present support
	VkBool32 present_support = VK_FALSE;
	vk->vkGetPhysicalDeviceSurfaceSupportKHR(vk->physical_device,
	                                          queue_family_index,
	                                          target->surface, &present_support);
	if (!present_support) {
		U_LOG_E("Queue family does not support presentation to Win32 surface");
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		free(target);
		return XRT_ERROR_VULKAN;
	}
#elif defined(XRT_OS_MACOS)
	// Create Metal surface via VK_EXT_metal_surface
	// hwnd parameter is actually a CAMetalLayer* on macOS
	// Try vk_bundle's pre-loaded function pointer first,
	// fall back to runtime lookup via vkGetInstanceProcAddr
	PFN_vkCreateMetalSurfaceEXT pfnCreateMetalSurface = vk->vkCreateMetalSurfaceEXT;
	if (pfnCreateMetalSurface == NULL) {
		pfnCreateMetalSurface = (PFN_vkCreateMetalSurfaceEXT)
		    vk->vkGetInstanceProcAddr(vk->instance, "vkCreateMetalSurfaceEXT");
	}
	if (pfnCreateMetalSurface == NULL) {
		U_LOG_E("vkCreateMetalSurfaceEXT not available — VK_EXT_metal_surface must be enabled");
		free(target);
		return XRT_ERROR_VULKAN;
	}

	U_LOG_I("Creating Metal surface from CAMetalLayer %p", hwnd);

	VkMetalSurfaceCreateInfoEXT surface_ci = {
	    .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
	    .pLayer = (const CAMetalLayer *)hwnd,
	};

	VkResult res = pfnCreateMetalSurface(vk->instance, &surface_ci, NULL, &target->surface);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create Metal surface: %d", res);
		free(target);
		return XRT_ERROR_VULKAN;
	}

	// Check present support
	VkBool32 present_support = VK_FALSE;
	vk->vkGetPhysicalDeviceSurfaceSupportKHR(vk->physical_device,
	                                          queue_family_index,
	                                          target->surface, &present_support);
	if (!present_support) {
		U_LOG_E("Queue family does not support presentation to Metal surface");
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		free(target);
		return XRT_ERROR_VULKAN;
	}
#elif defined(XRT_OS_ANDROID)
	if (vk->vkCreateAndroidSurfaceKHR == NULL) {
		U_LOG_E("vkCreateAndroidSurfaceKHR not loaded — VK_KHR_android_surface must be enabled");
		free(target);
		return XRT_ERROR_VULKAN;
	}
	if (hwnd == NULL) {
		U_LOG_E("VK native target: ANativeWindow* is NULL on Android");
		free(target);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	VkAndroidSurfaceCreateInfoKHR surface_ci = {
	    .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
	    .pNext = NULL,
	    .flags = 0,
	    .window = (ANativeWindow *)hwnd,
	};

	VkResult res = vk->vkCreateAndroidSurfaceKHR(vk->instance, &surface_ci, NULL, &target->surface);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create Android surface: %d", res);
		free(target);
		return XRT_ERROR_VULKAN;
	}

	VkBool32 present_support = VK_FALSE;
	vk->vkGetPhysicalDeviceSurfaceSupportKHR(vk->physical_device,
	                                          queue_family_index,
	                                          target->surface, &present_support);
	if (!present_support) {
		U_LOG_E("Queue family does not support presentation to Android surface");
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		free(target);
		return XRT_ERROR_VULKAN;
	}
#elif defined(XRT_OS_LINUX_DESKTOP)
#ifdef XRT_HAVE_WAYLAND
	if (is_wayland) {
		// App-provided Wayland surface (XR_DXR_wayland_surface_binding, WS3b):
		// build a VkWaylandSurfaceKHR from the wl_display* + wl_surface* pair.
		if (hwnd == NULL) {
			U_LOG_E("VK native target: Wayland handle is NULL");
			free(target);
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}
		const struct comp_vk_native_wayland_handle *wl_handle =
		    (const struct comp_vk_native_wayland_handle *)hwnd;
		PFN_vkCreateWaylandSurfaceKHR pfnCreateWaylandSurface =
		    (PFN_vkCreateWaylandSurfaceKHR)vk->vkGetInstanceProcAddr(vk->instance, "vkCreateWaylandSurfaceKHR");
		if (pfnCreateWaylandSurface == NULL) {
			U_LOG_E("vkCreateWaylandSurfaceKHR not available — VK_KHR_wayland_surface must be enabled");
			free(target);
			return XRT_ERROR_VULKAN;
		}
		VkWaylandSurfaceCreateInfoKHR wl_surface_ci = {
		    .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
		    .pNext = NULL,
		    .flags = 0,
		    .display = (struct wl_display *)wl_handle->display,
		    .surface = (struct wl_surface *)wl_handle->surface,
		};
		VkResult wl_res = pfnCreateWaylandSurface(vk->instance, &wl_surface_ci, NULL, &target->surface);
		if (wl_res != VK_SUCCESS) {
			U_LOG_E("Failed to create Wayland surface: %d", wl_res);
			free(target);
			return XRT_ERROR_VULKAN;
		}
		VkBool32 wl_present_support = VK_FALSE;
		vk->vkGetPhysicalDeviceSurfaceSupportKHR(vk->physical_device, queue_family_index, target->surface,
		                                         &wl_present_support);
		if (!wl_present_support) {
			U_LOG_E("Queue family does not support presentation to Wayland surface");
			vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
			free(target);
			return XRT_ERROR_VULKAN;
		}
	} else
#endif
	{
	// hwnd is a struct comp_vk_native_xcb_handle* carrying both the
	// xcb_connection_t* and the xcb_window_t (a single void* can't hold both).
	if (hwnd == NULL) {
		U_LOG_E("VK native target: XCB handle is NULL on Linux");
		free(target);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	const struct comp_vk_native_xcb_handle *xcb_handle =
	    (const struct comp_vk_native_xcb_handle *)hwnd;

	PFN_vkCreateXcbSurfaceKHR pfnCreateXcbSurface = vk->vkCreateXcbSurfaceKHR;
	if (pfnCreateXcbSurface == NULL) {
		pfnCreateXcbSurface = (PFN_vkCreateXcbSurfaceKHR)
		    vk->vkGetInstanceProcAddr(vk->instance, "vkCreateXcbSurfaceKHR");
	}
	if (pfnCreateXcbSurface == NULL) {
		U_LOG_E("vkCreateXcbSurfaceKHR not available — VK_KHR_xcb_surface must be enabled");
		free(target);
		return XRT_ERROR_VULKAN;
	}

	VkXcbSurfaceCreateInfoKHR surface_ci = {
	    .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
	    .pNext = NULL,
	    .flags = 0,
	    .connection = (xcb_connection_t *)xcb_handle->connection,
	    .window = (xcb_window_t)xcb_handle->window,
	};

	VkResult res = pfnCreateXcbSurface(vk->instance, &surface_ci, NULL, &target->surface);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create XCB surface: %d", res);
		free(target);
		return XRT_ERROR_VULKAN;
	}

	VkBool32 present_support = VK_FALSE;
	vk->vkGetPhysicalDeviceSurfaceSupportKHR(vk->physical_device,
	                                          queue_family_index,
	                                          target->surface, &present_support);
	if (!present_support) {
		U_LOG_E("Queue family does not support presentation to XCB surface");
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		free(target);
		return XRT_ERROR_VULKAN;
	}
	} // end XCB (non-Wayland) branch
#else
	U_LOG_E("VK native target: no supported surface type on this platform");
	free(target);
	return XRT_ERROR_DEVICE_CREATION_FAILED;
#endif

	// Create synchronization primitives
	VkSemaphoreCreateInfo sem_ci = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};
	VkResult vk_res;
	vk_res = vk->vkCreateSemaphore(vk->device, &sem_ci, NULL, &target->image_available);
	if (vk_res != VK_SUCCESS) {
		U_LOG_E("Failed to create image_available semaphore");
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		free(target);
		return XRT_ERROR_VULKAN;
	}
	vk_res = vk->vkCreateSemaphore(vk->device, &sem_ci, NULL, &target->render_finished);
	if (vk_res != VK_SUCCESS) {
		U_LOG_E("Failed to create render_finished semaphore");
		vk->vkDestroySemaphore(vk->device, target->image_available, NULL);
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		free(target);
		return XRT_ERROR_VULKAN;
	}

	// Create swapchain
	xrt_result_t xret = create_swapchain(target);
	if (xret != XRT_SUCCESS) {
		vk->vkDestroySemaphore(vk->device, target->render_finished, NULL);
		vk->vkDestroySemaphore(vk->device, target->image_available, NULL);
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		free(target);
		return xret;
	}

	*out_target = target;

	U_LOG_I("Created VK native target: %ux%u, %u images, format %d",
	        target->width, target->height, target->image_count, target->format);

	return XRT_SUCCESS;
}

xrt_result_t
comp_vk_native_target_create_from_surface(struct comp_vk_native_compositor *c,
                                          VkSurfaceKHR surface,
                                          uint32_t width,
                                          uint32_t height,
                                          struct comp_vk_native_target **out_target)
{
	// Direct-scanout path (ST-5539): the window backend already built the
	// display-plane surface (only it has the display/mode/plane context), so we
	// skip per-platform surface creation entirely and reuse the shared
	// semaphore + swapchain tail. The surface is borrowed, not owned — the
	// backend destroys it on its own teardown (external_surface = true).
	struct vk_bundle *vk = comp_vk_native_compositor_get_vk(c);
	uint32_t queue_family_index = comp_vk_native_compositor_get_queue_family(c);

	if (surface == VK_NULL_HANDLE) {
		U_LOG_E("VK native target: NULL surface handed to create_from_surface");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct comp_vk_native_target *target = U_TYPED_CALLOC(struct comp_vk_native_target);
	if (target == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	target->vk = vk;
	target->hwnd = NULL;
	target->width = width;
	target->height = height;
#ifdef XRT_OS_ANDROID
	target->last_display_landscape = -1; // unknown until a rect is published (#1074)
#endif
	target->queue_family_index = queue_family_index;
	target->surface = surface;
	target->external_surface = true;
	os_mutex_init(&target->swapchain_mutex);

	VkBool32 present_support = VK_FALSE;
	vk->vkGetPhysicalDeviceSurfaceSupportKHR(vk->physical_device, queue_family_index, target->surface,
	                                          &present_support);
	if (!present_support) {
		U_LOG_E("Queue family does not support presentation to the direct-scanout surface");
		free(target);
		return XRT_ERROR_VULKAN;
	}

	VkSemaphoreCreateInfo sem_ci = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};
	VkResult vk_res = vk->vkCreateSemaphore(vk->device, &sem_ci, NULL, &target->image_available);
	if (vk_res != VK_SUCCESS) {
		U_LOG_E("Failed to create image_available semaphore");
		free(target);
		return XRT_ERROR_VULKAN;
	}
	vk_res = vk->vkCreateSemaphore(vk->device, &sem_ci, NULL, &target->render_finished);
	if (vk_res != VK_SUCCESS) {
		U_LOG_E("Failed to create render_finished semaphore");
		vk->vkDestroySemaphore(vk->device, target->image_available, NULL);
		free(target);
		return XRT_ERROR_VULKAN;
	}

	xrt_result_t xret = create_swapchain(target);
	if (xret != XRT_SUCCESS) {
		vk->vkDestroySemaphore(vk->device, target->render_finished, NULL);
		vk->vkDestroySemaphore(vk->device, target->image_available, NULL);
		free(target);
		return xret;
	}

	*out_target = target;
	U_LOG_I("Created VK native direct-scanout target: %ux%u, %u images, format %d", target->width,
	        target->height, target->image_count, target->format);
	return XRT_SUCCESS;
}

void
comp_vk_native_target_destroy(struct comp_vk_native_target **target_ptr)
{
	if (target_ptr == NULL || *target_ptr == NULL) {
		return;
	}

	struct comp_vk_native_target *target = *target_ptr;
	struct vk_bundle *vk = target->vk;

#ifdef XRT_OS_WINDOWS
	// Stop the harness waiter BEFORE the swapchain goes away — an in-flight
	// vkWaitForPresentKHR on a destroyed swapchain is invalid.
	wl_teardown(target);
#endif

	vk->vkDeviceWaitIdle(vk->device);

#ifdef XRT_OS_WINDOWS
	if (target->dcomp_active) {
		dcomp_destroy(target);
		// dcomp_active path doesn't allocate semaphores / surface / swapchain.
		// The repaint thread was joined before we got here (see the destroy
		// order in comp_vk_native_compositor.c), so nothing can be blocked on
		// swapchain_mutex.
		os_mutex_destroy(&target->swapchain_mutex);
		free(target);
		*target_ptr = NULL;
		return;
	}
#endif

	destroy_swapchain_views(target);

	if (target->swapchain != VK_NULL_HANDLE) {
		vk->vkDestroySwapchainKHR(vk->device, target->swapchain, NULL);
	}
	if (target->render_finished != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, target->render_finished, NULL);
	}
	if (target->image_available != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, target->image_available, NULL);
	}
	if (target->surface != VK_NULL_HANDLE && !target->external_surface) {
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
	}

#ifdef XRT_OS_ANDROID
	// Release the ANativeWindow reference aux_android handed us (the one the
	// current VkSurfaceKHR was built from). #507
	if (target->android_window != NULL) {
		ANativeWindow_release((ANativeWindow *)target->android_window);
		target->android_window = NULL;
	}
#endif

	// See the dcomp branch above: the repaint thread is already joined.
	os_mutex_destroy(&target->swapchain_mutex);

	free(target);
	*target_ptr = NULL;
}

enum comp_vk_native_target_surface_state
comp_vk_native_target_sync_surface(struct comp_vk_native_target *target)
{
#ifndef XRT_OS_ANDROID
	(void)target;
	return COMP_VK_NATIVE_TARGET_SURFACE_READY;
#else
	struct vk_bundle *vk = target->vk;

	struct _ANativeWindow *cur_window = NULL;
	uint64_t cur_gen = 0;
	bool cur_valid = false;
	android_globals_get_window_state(&cur_window, &cur_gen, &cur_valid);

	// Surface unchanged since we (re)built — nothing to do here. The HW_XFORM
	// rotation-extent poll still runs in target_acquire for the live surface.
	if (cur_gen == target->surface_generation) {
		return target->surface_lost ? COMP_VK_NATIVE_TARGET_SURFACE_LOST
		                            : COMP_VK_NATIVE_TARGET_SURFACE_READY;
	}

	// The SurfaceView handed us a different surface (or lost it). Idle the GPU
	// and tear the old swapchain + VkSurfaceKHR down before touching the new one
	// — vkDestroySwapchainKHR must precede a new swapchain on the same window,
	// and we must not present to the dead surface.
	vk->vkDeviceWaitIdle(vk->device);
	destroy_swapchain_views(target);
	if (target->swapchain != VK_NULL_HANDLE) {
		vk->vkDestroySwapchainKHR(vk->device, target->swapchain, NULL);
		target->swapchain = VK_NULL_HANDLE;
	}
	if (target->surface != VK_NULL_HANDLE) {
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		target->surface = VK_NULL_HANDLE;
	}
	if (target->android_window != NULL) {
		// Release the reference aux_android handed us for the old window.
		ANativeWindow_release((ANativeWindow *)target->android_window);
		target->android_window = NULL;
	}

	target->surface_generation = cur_gen;

	if (!cur_valid || cur_window == NULL) {
		// Backgrounded: no live surface. Stay torn down; caller skips present so
		// the render thread never blocks on a dead window. #507
		target->surface_lost = true;
		U_LOG_I("Android output surface lost — target torn down (#507)");
		return COMP_VK_NATIVE_TARGET_SURFACE_LOST;
	}

	// Resume: rebuild VkSurfaceKHR + swapchain from the new ANativeWindow.
	// #1040: take our own reference, atomically with the read — `cur_window`
	// above is only safe to compare, a concurrent publish can free it.
	uint64_t acq_gen = 0;
	bool acq_valid = false;
	struct _ANativeWindow *win = android_globals_acquire_window(&acq_gen, &acq_valid);
	if (win == NULL || !acq_valid) {
		if (win != NULL) {
			ANativeWindow_release((ANativeWindow *)win);
		}
		target->surface_lost = true;
		return COMP_VK_NATIVE_TARGET_SURFACE_LOST;
	}
	target->surface_generation = acq_gen;

	VkAndroidSurfaceCreateInfoKHR surface_ci = {
	    .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
	    .pNext = NULL,
	    .flags = 0,
	    .window = (ANativeWindow *)win,
	};
	VkResult res = vk->vkCreateAndroidSurfaceKHR(vk->instance, &surface_ci, NULL, &target->surface);
	if (res != VK_SUCCESS) {
		U_LOG_E("sync_surface: vkCreateAndroidSurfaceKHR failed: %d", res);
		ANativeWindow_release((ANativeWindow *)win);
		target->surface = VK_NULL_HANDLE;
		target->surface_lost = true;
		return COMP_VK_NATIVE_TARGET_SURFACE_LOST;
	}
	target->android_window = (void *)win;

	// Adopt the new surface's current extent (covers a portrait<->landscape flip
	// that happened while backgrounded).
	VkSurfaceCapabilitiesKHR caps;
	if (vk->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->physical_device, target->surface, &caps) == VK_SUCCESS &&
	    caps.currentExtent.width != UINT32_MAX && caps.currentExtent.width != 0 &&
	    caps.currentExtent.height != 0) {
		target->width = caps.currentExtent.width;
		target->height = caps.currentExtent.height;
	}

	xrt_result_t xret = create_swapchain(target);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("sync_surface: create_swapchain failed after resume");
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		target->surface = VK_NULL_HANDLE;
		ANativeWindow_release((ANativeWindow *)target->android_window);
		target->android_window = NULL;
		target->surface_lost = true;
		return COMP_VK_NATIVE_TARGET_SURFACE_LOST;
	}

	target->surface_lost = false;
	// create_swapchain bumped target->generation: the DP is told about this
	// image set on the next weave (notify_target_recreated). This path used to
	// be the one rebuild that never bumped it -- see create_swapchain.
	U_LOG_I("Android output surface recreated %ux%u (#507), image set generation %u",
	        target->width, target->height, target->generation);
	return COMP_VK_NATIVE_TARGET_SURFACE_RECREATED;
#endif
}

/*!
 * #868/#902: pump the vblank grid from VK_GOOGLE_display_timing.
 *
 * Sits next to the present_wait probe above because it is the OTHER pacing
 * source, chosen by what the driver offers. Two reads, called after present:
 *
 *  - vkGetRefreshCycleDurationGOOGLE, once per swapchain — the MEASURED panel
 *    period. This is the half the open-loop path guessed at, and guessed wrong:
 *    the repaint loop announces 60 Hz while the platform runs the panel at 120.
 *  - vkGetPastPresentationTimingGOOGLE, every present — where presents ACTUALLY
 *    landed, which anchors the grid's phase.
 *
 * Deliberately best-effort and non-blocking: display timing is an optimisation
 * source, never a correctness dependency. Every failure path here leaves the
 * grid untrusted, and an untrusted grid makes its consumers keep exactly
 * today's behaviour rather than schedule against a fabricated clock.
 */
static void
target_feed_vblank_grid(struct comp_vk_native_target *target)
{
	struct vk_bundle *vk = target->vk;
	{
		// One-shot: say WHY the grid never populated. A silent bail here is
		// indistinguishable from "the extension works but the panel is idle",
		// which is a slow thing to diagnose from the outside.
		static bool announced = false;
		if (!announced) {
			announced = true;
			U_LOG_W("#902: display-timing feeder — vk=%d has_ext=%d swapchain=%d "
			        "get_past=%d get_refresh=%d",
			        vk != NULL, vk != NULL ? (int)vk->has_GOOGLE_display_timing : -1,
			        target->swapchain != VK_NULL_HANDLE,
			        (vk != NULL && vk->vkGetPastPresentationTimingGOOGLE != NULL),
			        (vk != NULL && vk->vkGetRefreshCycleDurationGOOGLE != NULL));
		}
	}
	if (vk == NULL || !vk->has_GOOGLE_display_timing || target->swapchain == VK_NULL_HANDLE) {
		return;
	}

	/*
	 * Re-probe the period PERIODICALLY, not once.
	 *
	 * The first version asked once per swapchain on the reasoning that the
	 * period cannot change without a recreate. That is wrong on this hardware:
	 * the panel is variable-refresh and the platform moves it between 60 and
	 * 120 on interaction, with no swapchain recreate involved. A once-only
	 * probe would latch whichever rate happened to be live at startup — and
	 * since the boost is touch-triggered, launching the app is exactly the
	 * moment most likely to sample the WRONG one.
	 *
	 * Every ~500 ms: far cheaper than a frame, and fast enough that a stale
	 * period cannot survive long enough to matter.
	 */
	const uint64_t probe_now_ns = os_monotonic_get_ns();
	const bool due = !target->refresh_cycle_probed ||
	                 (probe_now_ns - target->refresh_last_probe_ns) > (500ULL * 1000 * 1000);
	if (due && vk->vkGetRefreshCycleDurationGOOGLE != NULL) {
		target->refresh_cycle_probed = true;
		target->refresh_last_probe_ns = probe_now_ns;
		VkRefreshCycleDurationGOOGLE rc = {};
		if (vk->vkGetRefreshCycleDurationGOOGLE(vk->device, target->swapchain, &rc) == VK_SUCCESS) {
			const uint64_t prev = target->vblank_grid.period_ns;
			if (comp_vblank_grid_set_period(&target->vblank_grid, rc.refreshDuration)) {
				// Only on a CHANGE: this now runs twice a second, and a
				// per-probe line would bury the thing worth seeing, which is
				// the panel actually moving between rates.
				if (prev != target->vblank_grid.period_ns) {
					U_LOG_W("#902: display-timing grid — refresh %.3f ms (%.2f Hz)%s",
					        rc.refreshDuration / 1e6, 1e9 / (double)rc.refreshDuration,
					        prev == 0 ? " [initial]" : " [CHANGED]");
				}
			} else {
				U_LOG_W("#902: display-timing reported an implausible refresh "
				        "duration (%llu ns) — grid stays untrusted",
				        (unsigned long long)rc.refreshDuration);
			}
		}
	}

	if (vk->vkGetPastPresentationTimingGOOGLE == NULL) {
		return;
	}

	// Ask for the count first rather than guessing a fixed size, which would
	// silently truncate on a driver that batches more than expected.
	uint32_t count = 0;
	if (vk->vkGetPastPresentationTimingGOOGLE(vk->device, target->swapchain, &count, NULL) !=
	        VK_SUCCESS ||
	    count == 0) {
		return;
	}
	if (count > 16) {
		count = 16;
	}
	VkPastPresentationTimingGOOGLE timings[16] = {};
	if (vk->vkGetPastPresentationTimingGOOGLE(vk->device, target->swapchain, &count, timings) !=
	    VK_SUCCESS) {
		return;
	}

	/*
	 * TIMEBASE GUARD. The grid projects forward from these timestamps using
	 * os_monotonic_get_ns() as "now", which is only meaningful if the driver
	 * reports actualPresentTime in the SAME clock. The spec ties it to
	 * VkPresentTimeGOOGLE::desiredPresentTime, CLOCK_MONOTONIC on Android — but
	 * a driver reporting in another domain would leave the grid looking
	 * perfectly trusted and being entirely wrong, projecting vblanks into a
	 * clock nobody else uses. That is the single failure this module cannot
	 * detect on its own, so check it once, out loud, and refuse the feed
	 * rather than schedule against a clock we do not share.
	 */
	const uint64_t now_ns = os_monotonic_get_ns();
	if (!target->timebase_checked && count > 0 && timings[count - 1].actualPresentTime != 0) {
		target->timebase_checked = true;
		const uint64_t t = timings[count - 1].actualPresentTime;
		const int64_t delta_ms = ((int64_t)t - (int64_t)now_ns) / 1000000;
		// A present that already happened sits slightly BEHIND now. Anything
		// beyond a second either way is a different clock, not jitter.
		target->timebase_ok = (delta_ms > -1000 && delta_ms < 1000);
		U_LOG_W("#902: display-timing timebase check — actualPresentTime %llu vs "
		        "os_monotonic %llu (delta %lld ms) => %s",
		        (unsigned long long)t, (unsigned long long)now_ns, (long long)delta_ms,
		        target->timebase_ok ? "SAME CLOCK, grid usable"
		                            : "DIFFERENT CLOCK — grid refused");
	}
	if (target->timebase_checked && !target->timebase_ok) {
		return;
	}

	// Records can arrive out of order; observe() only ever moves the anchor
	// forward, so feeding them in whatever order the driver returns is safe.
	for (uint32_t i = 0; i < count; i++) {
		comp_vblank_grid_observe(&target->vblank_grid, timings[i].actualPresentTime);
	}
}

#ifdef XRT_OS_WINDOWS
/*
 * Lazily create the harness on first present (env-gated). Returns nullptr
 * when disabled or when vkWaitForPresentKHR is unavailable on this device
 * (extension/feature not enabled by the app).
 */
/*
 * Resolve vkWaitForPresentKHR once per target. Non-null only when the app
 * enabled the present_id/present_wait features at device creation.
 */
static PFN_vkWaitForPresentKHR
target_present_wait_fn(struct comp_vk_native_target *target)
{
	if (!target->present_wait_probed) {
		struct vk_bundle *vk = target->vk;
		target->present_wait_fn =
		    (PFN_vkWaitForPresentKHR)vk->vkGetDeviceProcAddr(vk->device, "vkWaitForPresentKHR");
		target->present_wait_probed = true;
		if (target->present_wait_fn == nullptr && (dxr_late_weave_enabled() || getenv("DXR_WEAVE_LATENCY_CSV"))) {
			U_LOG_W("vkWaitForPresentKHR unavailable (app did not enable "
			        "VK_KHR_present_wait) — late-weave pacing/harness dormant");
		}
	}
	return target->present_wait_fn;
}



static struct wl_harness *
wl_get(struct comp_vk_native_target *target)
{
	static int probed = -1; // process-wide env probe
	if (probed == 0) {
		return nullptr;
	}
	if (target->wl != nullptr) {
		return target->wl;
	}
	const char *prefix = getenv("DXR_WEAVE_LATENCY_CSV");
	if (probed < 0) {
		probed = (prefix != nullptr && prefix[0] != '\0') ? 1 : 0;
		if (probed == 0) {
			return nullptr;
		}
	}

	if (target_present_wait_fn(target) == nullptr) {
		probed = 0;
		return nullptr;
	}

	char path[512];
	snprintf(path, sizeof(path), "%s.vknative.csv", prefix);
	FILE *f = fopen(path, "a");
	if (f == nullptr) {
		probed = 0;
		return nullptr;
	}

	auto *wl = new wl_harness();
	wl->f = f;
	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	fprintf(f, "H,%lld\n", (long long)freq.QuadPart);

	// Waiter: timestamps each present id hitting glass. 100 ms wait slices so
	// teardown/recreate never blocks long on an in-flight wait.
	PFN_vkWaitForPresentKHR wait_fn = target->present_wait_fn;
	wl->waiter = std::thread([target, wl, wait_fn]() {
		struct vk_bundle *vk = target->vk;
		for (;;) {
			uint64_t id = 0;
			{
				std::unique_lock<std::mutex> lock(wl->mtx);
				wl->cv.wait(lock, [wl]() { return wl->stop || !wl->ids.empty(); });
				if (wl->stop) {
					return;
				}
				id = wl->ids.front();
				wl->ids.pop_front();
			}
			for (int i = 0; i < 20; i++) { // ≤2 s per id, sliced
				// Same lifetime guard as comp_vk_native_target_repaint_pace:
				// this thread has no compositor lock either, and a resize can
				// null target->swapchain underneath it. Held per slice only —
				// wl_teardown joins this thread and must never be called with
				// swapchain_mutex held.
				os_mutex_lock(&target->swapchain_mutex);
				if (target->recreate_pending || target->swapchain == VK_NULL_HANDLE) {
					os_mutex_unlock(&target->swapchain_mutex);
					break;
				}
				VkResult r = wait_fn(vk->device, target->swapchain, id, 100ull * 1000 * 1000);
				os_mutex_unlock(&target->swapchain_mutex);
				if (r == VK_TIMEOUT && !wl->stop) {
					continue;
				}
				if (r == VK_SUCCESS) {
					LARGE_INTEGER now;
					QueryPerformanceCounter(&now);
					std::lock_guard<std::mutex> lock(wl->mtx);
					fprintf(wl->f, "S,%llu,0,0,%llu,%llu\n", (unsigned long long)id,
					        (unsigned long long)now.QuadPart, (unsigned long long)now.QuadPart);
					fflush(wl->f);
				}
				break;
			}
		}
	});

	target->wl = wl;
	// #1044: the DComp bridge never presents through the VK swapchain the
	// waiter watches (present dispatches to dcomp_present before the F-row
	// writer), so on that path this CSV gets a header and nothing else. The
	// bridge is set up in target_create, before any mark/present can reach
	// this first wl_get, so dcomp_active is already settled here — one-shot
	// by construction (wl_get creates the harness once per target). Never
	// claim "active" on that path: the first line is the one a log reader
	// takes as the verdict (measured — the avatar dGPU ladder read the old
	// "active" line as a working instrument over an empty CSV).
	if (target->dcomp_active) {
		U_LOG_W(
		    "Weave-latency harness: ARMED-DORMANT — DComp-bridge presentation "
		    "never reaches the VK swapchain the waiter watches; %s will "
		    "contain a header and NO rows (#1044). Use DXR_FRAME_WITNESS=1 "
		    "for rate counters on this path.",
		    path);
	} else {
		U_LOG_W("Weave-latency harness: VK present_wait scanout timing active (%s)", path);
	}
	return wl;
}

static void
wl_teardown(struct comp_vk_native_target *target)
{
	struct wl_harness *wl = target->wl;
	if (wl == nullptr) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(wl->mtx);
		wl->stop = true;
	}
	wl->cv.notify_all();
	if (wl->waiter.joinable()) {
		wl->waiter.join();
	}
	// #1044: an armed harness that measured nothing must say so — a silent
	// header-only CSV reads as "no latency problem" instead of "cannot
	// measure on this presentation path".
	if (wl->rows_written == 0) {
		U_LOG_W(
		    "Weave-latency harness: armed but wrote zero F rows — this "
		    "presentation path cannot measure scanout (#1044)");
	}
	fclose(wl->f);
	delete wl;
	target->wl = nullptr;
}
#endif // XRT_OS_WINDOWS

void
comp_vk_native_target_weave_mark_repaint(struct comp_vk_native_target *target, bool mode_3d)
{
	g_frame_witness_vk.count_weave(true, mode_3d);
#ifdef XRT_OS_WINDOWS
	/*
	 * Deliberately NOT the app-frame mark. A repaint updates only
	 * weave_mark_qpc (so the harness can still timestamp this weave's own
	 * scanout) and leaves last_mark_qpc, wait_frame_qpc, interval_ema_ns and
	 * wait_to_weave_ema_ns alone — those describe the APP's cadence, and a
	 * repaint is not an app frame.
	 */
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	target->weave_mark_qpc = (uint64_t)now.QuadPart;
	struct wl_harness *wl = wl_get(target);
	if (wl != nullptr) {
		wl->pending_weave_qpc = (uint64_t)now.QuadPart;
		wl->pending_repaint = true; // #1048: this weave is a repaint — tag its F row
	}
	// #912/#868 interplay: this repaint's present will release a bridge
	// waitable token nobody waits for; the app's next acquire drains it.
	g_vk_repaint_presents_since_app.fetch_add(1, std::memory_order_relaxed);
#else
	(void)target;
#endif
}

void
comp_vk_native_target_repaint_pace(struct comp_vk_native_target *target)
{
#ifdef XRT_OS_WINDOWS
	/*
	 * Scanout-only pacing. See the header for why this must never wait on an
	 * acquire/frame-latency token. When present_wait is unavailable (Intel
	 * iGPUs expose no VK present-timing extensions) there is nothing to wait
	 * on and the repaint thread's own sleep does the pacing.
	 */
	PFN_vkWaitForPresentKHR wait_fn = target_present_wait_fn(target);
	if (wait_fn == nullptr || target->last_present_id == 0) {
		return;
	}

	/*
	 * SLICED, and under swapchain_mutex — this is the fullscreen-crash site.
	 *
	 * This runs on the repaint thread WITHOUT the compositor lock (by design:
	 * see the header). A window resize on the app thread nulls
	 * target->swapchain inside comp_vk_native_target_resize, and a single
	 * 100 ms wait straddling that gave the ICD a VK_NULL_HANDLE swapchain →
	 * SRW lock on null → hard process death. Fullscreen is simply the resize
	 * that always lands mid-pace.
	 *
	 * The mutex alone would be correct but would make a recreate wait out the
	 * whole 100 ms. Slicing keeps the total budget identical while bounding
	 * how long a recreate can be held off to ONE slice — the same trade the
	 * harness waiter already makes (see wl_setup).
	 */
	const uint64_t total_ns = 100ull * 1000 * 1000;
	const uint64_t slice_ns = 12ull * 1000 * 1000;
	for (uint64_t waited_ns = 0; waited_ns < total_ns; waited_ns += slice_ns) {
		os_mutex_lock(&target->swapchain_mutex);
		// Re-read under the lock every slice: a recreate between slices
		// replaces the handle, and the id sequence restarts at 0 with it.
		if (target->recreate_pending || target->swapchain == VK_NULL_HANDLE ||
		    target->last_present_id == 0) {
			os_mutex_unlock(&target->swapchain_mutex);
			return;
		}
		VkResult wres =
		    wait_fn(target->vk->device, target->swapchain, target->last_present_id, slice_ns);
		os_mutex_unlock(&target->swapchain_mutex);
		if (wres != VK_TIMEOUT) {
			break; // presented (or the swapchain went out of date) — stop waiting
		}
	}

	/*
	 * LATE-weave, not early-weave.
	 *
	 * The wait above returns just after the PREVIOUS frame hit glass, which is
	 * the START of a refresh period — weaving there would sample the eyes a
	 * whole period before the pixels are actually scanned out, i.e. the
	 * staleness this feature exists to remove.
	 *
	 * The next scanout is one period away, and `measured_r_ns` is how long a
	 * weave takes to reach glass. So the last safe moment to begin is
	 * period - R. Sleeping that much converts the repaint from "as early as
	 * possible" to "as late as still lands", which is the whole point of
	 * decoupling weave rate from render rate: the eye position used is the
	 * freshest one that can still make the frame.
	 *
	 * Clamped hard. With no measurement yet (R = 0) we would sleep a full
	 * period and miss the frame entirely, so a floor of a quarter period is
	 * kept in hand, and anything that would not fit simply weaves immediately.
	 * A repaint that arrives late is a dropped repaint; a repaint that arrives
	 * early is merely stale. Never gamble the frame.
	 */
	const double period_ns = target->period_hint_ns;
	const double residual_ns = (double)target->measured_r_ns;
	if (period_ns <= 0.0 || residual_ns <= 0.0) {
		return; // unmeasured — weave now rather than guess
	}
	double slack_ns = period_ns - residual_ns;
	const double floor_ns = period_ns * 0.25; // never cut it finer than this
	if (slack_ns > period_ns - floor_ns) {
		slack_ns = period_ns - floor_ns;
	}
	if (slack_ns <= 0.0) {
		return; // the weave already fills the period; go now
	}
	os_nanosleep((int64_t)slack_ns);
#else
	(void)target;
#endif
}

void
comp_vk_native_target_weave_mark(struct comp_vk_native_target *target, bool mode_3d)
{
	g_frame_witness_vk.count_weave(false, mode_3d);
#ifdef XRT_OS_WINDOWS
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	// #867/#912: the #850 governor is now the single owner of the frame-cost
	// and wait_frame→weave EMAs (its on_mark carries the identical constants,
	// the 250 ms pause reset, and — new here — the saturation depth logic for
	// the bridge pacing). The hand-rolled duplicates this replaced would have
	// drifted from it. App frames only: weave_mark_repaint deliberately never
	// calls on_mark (a repaint is not an app frame; its one-period cadence
	// would collapse the saturation signal).
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	const int tr = g_lw_gov_vk_bridge.on_mark((uint64_t)f.QuadPart);
	// drag_shallow: while a drag clamps the queue to 1, do not let a governor
	// transition re-deepen it mid-drag — the clamp owner restores depth.
	if (tr != 0 && target->dcomp_frame_latency_waitable != NULL && !target->drag_shallow &&
	    dxr_vk_bridge_pacing_mode() == DXR_VK_BRIDGE_PACING_GOVERNOR) {
		IDXGISwapChain2 *sc2 = NULL;
		if (target->dcomp_swapchain != NULL &&
		    SUCCEEDED(target->dcomp_swapchain->QueryInterface(__uuidof(IDXGISwapChain2),
		                                                       (void **)&sc2)) &&
		    sc2 != NULL) {
			sc2->SetMaximumFrameLatency((UINT)g_lw_gov_vk_bridge.effective);
			sc2->Release();
			target->dcomp_pacing_depth = g_lw_gov_vk_bridge.effective;
		}
		static bool logged_backoff = false, logged_return = false;
		bool &logged = (tr > 0) ? logged_backoff : logged_return;
		if (!logged) {
			logged = true;
			U_LOG_W("#912: bridge saturation %s -> max latency %d "
			        "(frame interval %.1f ms vs display period %.1f ms)",
			        tr > 0 ? "backoff engaged" : "cleared, probing return",
			        g_lw_gov_vk_bridge.effective, g_lw_gov_vk_bridge.interval_ema_ns / 1e6,
			        g_lw_gov_vk_bridge.period_ns / 1e6);
		}
	}
	target->weave_mark_qpc = (uint64_t)now.QuadPart; // always: feeds set_frame_timing
	struct wl_harness *wl = wl_get(target);
	if (wl != nullptr) {
		wl->pending_weave_qpc = (uint64_t)now.QuadPart;
		wl->pending_repaint = false; // #1048: an app frame, not a repaint
	}
#else
	(void)target;
#endif
}

void
comp_vk_native_target_mark_wait_frame(struct comp_vk_native_target *target)
{
#ifdef XRT_OS_WINDOWS
	(void)target;
	g_lw_gov_vk_bridge.on_wait_frame(); // #912: governor owns the EMAs now
#else
	(void)target;
#endif
}

void
comp_vk_native_target_set_display_period(struct comp_vk_native_target *target, uint64_t period_ns)
{
#ifdef XRT_OS_WINDOWS
	target->period_hint_ns = (double)period_ns;
	// #912: seed the governor's saturation reference — on_stats never fires
	// on the bridge (frame statistics are DISJOINT), so this is its only
	// period source.
	if (g_lw_gov_vk_bridge.period_ns <= 0.0 && period_ns > 0) {
		g_lw_gov_vk_bridge.period_ns = (double)period_ns;
	}
#else
	(void)target;
	(void)period_ns;
#endif
}

void
comp_vk_native_target_note_origin_motion(struct comp_vk_native_target *target)
{
#ifdef XRT_OS_WINDOWS
	// See drag_shallow in the target struct. 300 ms: a live drag refreshes
	// this every frame; one dwell past the last movement restores depth.
	target->origin_motion_until_ns = os_monotonic_get_ns() + 300ull * 1000000ull;
#else
	(void)target;
#endif
}

uint64_t
comp_vk_native_target_get_predicted_lookahead_ns(struct comp_vk_native_target *target)
{
#ifdef XRT_OS_WINDOWS
	return late_weave_lookahead_ns(g_lw_gov_vk_bridge.wait_to_weave_ema_ns, target->measured_r_ns,
	                               target->period_hint_ns);
#else
	(void)target;
	return 0;
#endif
}

uint64_t
comp_vk_native_target_get_measured_weave_ns(struct comp_vk_native_target *target)
{
#ifdef XRT_OS_WINDOWS
	return target->measured_r_ns;
#else
	(void)target;
	return 0;
#endif
}

xrt_result_t
comp_vk_native_target_acquire(struct comp_vk_native_target *target, uint32_t *out_index, VkQueue queue, bool is_repaint)
{
	struct vk_bundle *vk = target->vk;

	// #1236: refuse to enter the driver with a destroyed swapchain. A failed
	// recreate below leaves this NULL, and vkAcquireNextImageKHR(…, VK_NULL_HANDLE, …)
	// faults inside the Adreno driver (SIGSEGV at 0x0 in AcquireNextImageKHR, reached
	// from vk_repaint_thread). Every caller already handles the error return, so
	// failing here is strictly better than a null dereference one frame later.
	//
	// EXCEPTION — the Windows DComp bridge. On that path VK never goes through WSI
	// at all (see the @ref dcomp_active doc block): the compositor renders into the
	// imported ring and the branch below returns a ring index without ever calling
	// vkAcquireNextImageKHR, so target->swapchain is legitimately VK_NULL_HANDLE.
	// Guarding it there fails EVERY frame of EVERY transparent-window app.
	bool needs_vk_swapchain = true;
#ifdef XRT_OS_WINDOWS
	needs_vk_swapchain = !target->dcomp_active;
#endif
	if (needs_vk_swapchain && target->swapchain == VK_NULL_HANDLE) {
#ifdef XRT_OS_ANDROID
		// Latch so sync_surface reports LOST and the compositor skips frames until
		// the next surface generation, rather than retrying into the same hole.
		// Honours the same kill switch as the other two latch sites. Note the
		// GUARD above is not optional and is deliberately outside it: refusing a
		// NULL acquire IS the crash fix. The switch only controls whether we also
		// latch, so `debug.dxr.surface_lost_latch 0` means what it says.
		if (dxr_surface_lost_latch_enabled() && !target->surface_lost) {
			target->surface_lost = true;
			U_LOG_W("acquire with no swapchain — surface latched LOST until the next "
			        "generation (#1236)");
		}
#else
		U_LOG_E("acquire with no swapchain (#1236)");
#endif
		return XRT_ERROR_VULKAN;
	}

#ifdef XRT_OS_WINDOWS
	if (target->dcomp_active) {
		// #870 — late-weave pacing for the bridge. Block until DXGI says the
		// previous present has been consumed, so the frame that follows
		// (record → weave with a fresh eye prediction → present) starts on a
		// known phase instead of free-running. This is the fallback for
		// drivers with no VK_KHR_present_wait — i.e. every Intel iGPU, which
		// is the adapter that scans out the panel — and it is deliberately
		// skipped when Vulkan's own mechanism is available, so nothing
		// double-paces on NVIDIA. Bounded wait: a lost/occluded present must
		// not wedge the render thread.
		//
		// INVARIANT — ONLY THE PRESENTING THREAD MAY WAIT ON THIS HANDLE.
		// The DXGI frame-latency waitable is a SEMAPHORE, not an event: every
		// wait consumes a token that a present is supposed to replace. A
		// second waiter (a repaint pacer, a stats thread, a "just check if
		// we're behind" poll) steals tokens from the present loop and starves
		// it — measured elsewhere in this runtime as 31.9 -> 16.9 fps while
		// the offending thread issued essentially zero presents, with every
		// counter reading zero because nothing looked wrong. This wait is
		// safe only because acquire and present are both called from
		// vk_compositor_layer_commit, i.e. the same thread, once per frame:
		// one wait, one present. If you add a path that presents WITHOUT
		// acquiring (e.g. re-presenting the last atlas), do not reuse this
		// wait — either pair each extra present with its own wait on this
		// same thread, or pace that path passively (frame statistics /
		// present-wait polling) and never touch this handle.
		// #912: repaints NEVER touch the waitable (the invariant above — and
		// on tier-2 single-queue iGPUs the repaint shares the app's VkQueue,
		// so the queue cannot discriminate; the caller passes is_repaint).
		if (!is_repaint && target->dcomp_frame_latency_waitable != NULL &&
		    target_present_wait_fn(target) == NULL) {
			static bool logged = false;
			if (!logged) {
				logged = true;
				U_LOG_W("#912: pacing the VK bridge on the DXGI waitable, depth %d%s "
				        "(no VK_KHR_present_wait on this device)",
				        target->dcomp_pacing_depth,
				        (dxr_vk_bridge_pacing_mode() == DXR_VK_BRIDGE_PACING_GOVERNOR)
				            ? " + saturation auto-backoff"
				            : " (pinned)");
			}
			// #868 interplay: repaint presents released tokens nobody
			// consumed — drain the surplus or this wait returns instantly
			// for that many frames after every idle stretch.
			const uint32_t rp =
			    g_vk_repaint_presents_since_app.exchange(0, std::memory_order_relaxed);
			if (rp > 0) {
				uint32_t drained = 0;
				while (drained < rp + LATE_WEAVE_MAX_DEPTH &&
				       WaitForSingleObjectEx(target->dcomp_frame_latency_waitable, 0, TRUE) ==
				           WAIT_OBJECT_0) {
					drained++;
				}
				static bool logged_drain = false;
				if (!logged_drain && drained > 2) {
					logged_drain = true;
					U_LOG_W("#912: drained %u surplus bridge tokens after repaints "
					        "(#868 interplay; logged once)",
					        drained);
				}
			}
			// Drag-shallow (see the struct field): clamp the queue to 1 while
			// the present origin is moving, restore governor depth after.
			const bool origin_moving =
			    os_monotonic_get_ns() < target->origin_motion_until_ns;
			if (origin_moving != target->drag_shallow) {
				IDXGISwapChain2 *ds2 = NULL;
				if (target->dcomp_swapchain != NULL &&
				    SUCCEEDED(target->dcomp_swapchain->QueryInterface(
				        __uuidof(IDXGISwapChain2), (void **)&ds2)) &&
				    ds2 != NULL) {
					// Governor mode: restore to the CURRENT effective —
					// a transition that fired mid-drag was skipped and
					// would otherwise be lost.
					int restore = (dxr_vk_bridge_pacing_mode() ==
					               DXR_VK_BRIDGE_PACING_GOVERNOR)
					                  ? g_lw_gov_vk_bridge.effective
					                  : target->dcomp_pacing_depth;
					if (restore < 1) {
						restore = 1;
					}
					if (!origin_moving) {
						target->dcomp_pacing_depth = restore;
					}
					ds2->SetMaximumFrameLatency(origin_moving ? 1 : (UINT)restore);
					ds2->Release();
					if (origin_moving) {
						// Reducing the cap does not revoke banked
						// tokens — drain them, or the next waits
						// return instantly and the queue stays deep.
						int drained = 0;
						while (drained < LATE_WEAVE_MAX_DEPTH &&
						       WaitForSingleObjectEx(
						           target->dcomp_frame_latency_waitable, 0,
						           TRUE) == WAIT_OBJECT_0) {
							drained++;
						}
					}
					target->drag_shallow = origin_moving;
					static bool logged_shallow = false;
					if (origin_moving && !logged_shallow) {
						logged_shallow = true;
						U_LOG_W("#912: origin moving (window drag) — bridge "
						        "queue clamped to 1 for phase snap; "
						        "governor depth restored when the drag ends "
						        "(logged once)");
					}
				}
			}
			const uint64_t wait_t0 = os_monotonic_get_ns();
			const DWORD wr =
			    WaitForSingleObjectEx(target->dcomp_frame_latency_waitable, 100, TRUE);
			const uint64_t wait_t1 = os_monotonic_get_ns();
			const bool wait_blocked = (wait_t1 - wait_t0) > 2000000; // >2 ms
			// Catch-up guard: if the LAST frame overran its period (GPU
			// contention pushed the weave past a tick) the pipeline is
			// already a vsync behind — tick-aligning now waits for yet
			// another tick and turns one late pickup into a dropped frame.
			// Measured on the iGPU: align-always put 20% of frames at
			// 2 vsyncs (60 -> 51 fps) with identical per-frame GPU cost.
			// Skip the align once; the queue slack absorbs the late frame
			// and the next on-time frame re-aligns.
			static uint64_t s_last_pace_done_ns;
			const double cu_period_ns =
			    (target->period_hint_ns > 0.0) ? target->period_hint_ns : 16.7e6;
			const bool running_late =
			    s_last_pace_done_ns != 0 &&
			    (double)(wait_t1 - s_last_pace_done_ns) > cu_period_ns * 1.15;
			if (wr == WAIT_TIMEOUT) {
				// Occlusion, not saturation (see on_wait_timeout).
				g_lw_gov_vk_bridge.on_wait_timeout();
			} else if (!wait_blocked && !running_late && !origin_moving &&
			           g_lw_gov_vk_bridge.align_ok()) {
				// Instant release (banked token, unknown phase): align to
				// the DComp compositor tick so the weave gets a constant
				// phase. A BLOCKING release is already tick-aligned — a
				// clock wait on top adds a whole period (measured on the
				// D3D legs: 2x-period intervals + spurious backoff).
				late_weave_wait_compositor_clock(target->period_hint_ns);
			}
			s_last_pace_done_ns = os_monotonic_get_ns();
		}
		// Round-robin through the bridge ring. No WSI to acquire from.
		// vkQueueWaitIdle in the compositor's render path covers the GPU
		// fence; D3D11's IDXGIKeyedMutex::AcquireSync in dcomp_present
		// flushes the writer caches before D3D11 reads the shared resource.
		target->dcomp_ring_idx = (target->dcomp_ring_idx + 1) % DCOMP_RING;
		target->current_index = target->dcomp_ring_idx;
		*out_index = target->current_index;
		return XRT_SUCCESS;
	}
#endif

#ifdef XRT_OS_ANDROID
	// LOXR-730/733: Adreno does NOT report VK_ERROR_OUT_OF_DATE_KHR on device
	// rotation, so without this the swapchain would stay at the launch
	// orientation's extent forever (portrait 1600x2560 <-> landscape 2560x1600).
	// The Leia weave must map 1:1 to the panel, so a stale extent breaks the
	// weave in whichever orientation we did NOT launch in. Poll the surface
	// extent each acquire and recreate the target (with preTransform=IDENTITY)
	// when it flips. vkGetPhysicalDeviceSurfaceCapabilitiesKHR is cheap.
	{
		VkSurfaceCapabilitiesKHR caps;
		VkResult cres = vk->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
		    vk->physical_device, target->surface, &caps);
		if (cres == VK_SUCCESS && caps.currentExtent.width != UINT32_MAX &&
		    caps.currentExtent.width != 0 && caps.currentExtent.height != 0 &&
		    (caps.currentExtent.width != target->width ||
		     caps.currentExtent.height != target->height)) {
			// #1074: the extent changing is NOT proof the DEVICE rotated.
			// In a freeform / split-screen window the surface follows the
			// WINDOW, so an aspect-crossing resize (1000x1500 -> 1500x1000)
			// swaps the extent on a display that never moved. Ask the
			// display, whose extent in the current rotation rides along
			// with the window rect (#1034). Diagnostic only here — this leg
			// recreates on ANY extent change, which is what a resize needs;
			// what was wrong was calling every one of them a rotation.
			bool disp_landscape = false;
			const char *why = "window resize";
			if (android_globals_get_display_landscape(&disp_landscape)) {
				const int now_landscape = disp_landscape ? 1 : 0;
				if (target->last_display_landscape >= 0 &&
				    target->last_display_landscape != now_landscape) {
					why = "device rotation";
				}
				target->last_display_landscape = now_landscape;
			} else {
				// Nothing published a rect (no window-geometry extension):
				// no rotation signal exists, so say so rather than guess.
				why = "extent change";
			}
			U_LOG_W("HW_XFORM: surface extent %ux%u -> %ux%u (%s), recreating target",
			        target->width, target->height, caps.currentExtent.width,
			        caps.currentExtent.height, why);
			xrt_result_t rret = comp_vk_native_target_resize(
			    target, caps.currentExtent.width, caps.currentExtent.height);
			if (rret != XRT_SUCCESS) {
				U_LOG_E("Failed to recreate target swapchain on rotation");
				return rret;
			}
		}
	}
#endif

#ifdef XRT_OS_WINDOWS
	// Late-weave pacing: block until the PREVIOUS present hit glass before
	// starting this frame. Everything downstream (record, weave with a fresh
	// eye prediction, submit, present) then runs inside the current refresh
	// interval and the FIFO present queue never accumulates depth — the VK
	// twin of the D3D11 waitable-swapchain pacing. Timeout covers occluded
	// windows where presents stop reaching glass.
	if (dxr_late_weave_enabled() && target->last_present_id != 0) {
		PFN_vkWaitForPresentKHR wait_fn = target_present_wait_fn(target);
		if (wait_fn != nullptr) {
			// #850: at depth L>1 pace on the present L-1 back instead of
			// the latest one, restoring L-1 frames of overlap.
			const uint64_t relax = (uint64_t)(vk_late_weave_max_latency() - 1);
			const uint64_t wait_id =
			    target->last_present_id > relax ? target->last_present_id - relax : 0;
			if (wait_id != 0) {
				VkResult wres =
				    wait_fn(vk->device, target->swapchain, wait_id, 100ull * 1000 * 1000);
				const uint64_t weave_qpc =
				    (target->present_qpc_ring_id[wait_id % 8] == wait_id)
				        ? target->present_qpc_ring[wait_id % 8]
				        : 0;
				if (wres == VK_SUCCESS && weave_qpc != 0) {
					LARGE_INTEGER now2, freq2;
					QueryPerformanceCounter(&now2);
					QueryPerformanceFrequency(&freq2);
					target->measured_r_ns = (uint64_t)(
					    (double)((uint64_t)now2.QuadPart - weave_qpc) *
					    1000000000.0 / (double)freq2.QuadPart);
				}
			}
		}
	}
#endif

	// Use the semaphore for acquire, then do a dummy submit that waits on it
	// to ensure the image is actually available before the compositor renders.
	VkResult res = vk->vkAcquireNextImageKHR(vk->device, target->swapchain,
	                                          UINT64_MAX, target->image_available,
	                                          VK_NULL_HANDLE, &target->current_index);
	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
		// Swapchain invalidated (window resize, minimize, etc.) — recreate and retry
		U_LOG_I("Swapchain out of date, recreating");

#ifdef XRT_OS_WINDOWS
		// Present ids are per-swapchain; drop the harness (it lazily
		// re-arms on the new swapchain) and reset the id sequence.
		// MUST precede the guard: wl_teardown joins a thread that takes
		// swapchain_mutex.
		wl_teardown(target);
		target->present_id_counter = 0;
		target->last_present_id = 0;
#endif

		// #902: the grid's PHASE is meaningless across a new swapchain, but
		// the panel's period usually is not — reset_phase keeps the period so
		// the grid re-anchors on the first present instead of re-probing.
		// refresh_cycle_probed clears too: a recreate can follow a mode change.
		comp_vblank_grid_reset_phase(&target->vblank_grid);
		target->refresh_cycle_probed = false;

		{
			// Same lifetime guard as comp_vk_native_target_resize. This
			// path runs under the compositor lock, but the compositor
			// lock is exactly what the unlocked pacing does NOT hold —
			// and either recreate path can be the one that nulls the
			// handle under it.
			target_recreate_guard guard(target);

			vk->vkDeviceWaitIdle(vk->device);
			destroy_swapchain_views(target);

			// Destroy old swapchain BEFORE creating new one — MoltenVK requires
			// the native window to be free (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR).
			if (target->swapchain != VK_NULL_HANDLE) {
				vk->vkDestroySwapchainKHR(vk->device, target->swapchain, NULL);
				target->swapchain = VK_NULL_HANDLE;
			}

			xrt_result_t xret = create_swapchain(target);
			if (xret != XRT_SUCCESS) {
				// #1236: the old swapchain is already destroyed, so returning
				// here leaves target->swapchain NULL. On Android that is the
				// backgrounding race — the surface died between the
				// OUT_OF_DATE and this recreate — so latch, exactly as the
				// VK_ERROR_SURFACE_LOST_KHR path below does. Without the latch
				// the repaint thread comes straight back and acquires from a
				// NULL swapchain.
				U_LOG_E("Failed to recreate swapchain");
#ifdef XRT_OS_ANDROID
				if (dxr_surface_lost_latch_enabled() && !target->surface_lost) {
					target->surface_lost = true;
					U_LOG_W("swapchain recreate failed — output surface latched "
					        "LOST until the next surface generation (#1236)");
				}
#endif
				return XRT_ERROR_VULKAN;
			}
		}

		// Retry acquire with new swapchain
		res = vk->vkAcquireNextImageKHR(vk->device, target->swapchain,
		                                 UINT64_MAX, target->image_available,
		                                 VK_NULL_HANDLE, &target->current_index);
		if (res != VK_SUCCESS) {
			U_LOG_E("Failed to acquire after swapchain recreation: %d", res);
#ifdef XRT_OS_ANDROID
			// #1236: same reasoning as the primary acquire path — a surface that
			// died under the recreate is unrecoverable without a new generation.
			if (res == VK_ERROR_SURFACE_LOST_KHR && dxr_surface_lost_latch_enabled() &&
			    !target->surface_lost) {
				target->surface_lost = true;
			}
#endif
			return XRT_ERROR_VULKAN;
		}
	} else if (res != VK_SUCCESS) {
#ifdef XRT_OS_ANDROID
		// #1146 secondary: the SurfaceView's surface is gone but the #507 poll
		// has not published the clear yet. Latch, so comp_vk_native_target_sync_surface
		// returns SURFACE_LOST on the following frames and the compositor skips
		// them — instead of hammering an abandoned BufferQueue at frame rate.
		// The latch is released by the next generation bump (clear or resume),
		// which is the same recovery edge sync_surface already implements.
		//
		// Narrow on purpose: only VK_ERROR_SURFACE_LOST_KHR is definitionally
		// unrecoverable without a new surface. Any other code keeps the old
		// retry-every-frame behaviour, so a transient failure can still heal.
		if (res == VK_ERROR_SURFACE_LOST_KHR && dxr_surface_lost_latch_enabled() && !target->surface_lost) {
			target->surface_lost = true;
			U_LOG_W(
			    "Failed to acquire swapchain image: %d — output surface latched LOST until the "
			    "next surface generation (#1146; setprop debug.dxr.surface_lost_latch 0 to "
			    "disable)",
			    res);
			return XRT_ERROR_VULKAN;
		}
#endif
		U_LOG_E("Failed to acquire swapchain image: %d", res);
		return XRT_ERROR_VULKAN;
	}

	// Wait for the acquired image to be available by doing a dummy submit
	// that waits on the image_available semaphore.
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkSubmitInfo wait_submit = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .waitSemaphoreCount = 1,
	    .pWaitSemaphores = &target->image_available,
	    .pWaitDstStageMask = &wait_stage,
	};
	// #868: whichever queue the CALLER owns — the app frame uses the app's,
	// a repaint uses the runtime-owned one. Never mix: a VkQueue is externally
	// synchronised and the runtime cannot serialise the app's own submits.
	vk->vkQueueSubmit(queue, 1, &wait_submit, VK_NULL_HANDLE);
	vk->vkQueueWaitIdle(queue);

	*out_index = target->current_index;
	return XRT_SUCCESS;
}

xrt_result_t
comp_vk_native_target_present(struct comp_vk_native_target *target, VkQueue queue)
{
	struct vk_bundle *vk = target->vk;

	// Witness counts at the dispatch point so the DComp-bridge path (which
	// never reaches the WSI present below) is counted too — the whole point
	// of the witness is holding on paths the latency harness cannot see.
	g_frame_witness_vk.count_present();

#ifdef XRT_OS_WINDOWS
	if (target->dcomp_active) {
		return dcomp_present(target);
	}
#endif

	// No semaphore wait needed — the compositor calls vkQueueWaitIdle
	// after all rendering commands before presenting.
	VkPresentInfoKHR present_info = {
	    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
	    .waitSemaphoreCount = 0,
	    .pWaitSemaphores = NULL,
	    .swapchainCount = 1,
	    .pSwapchains = &target->swapchain,
	    .pImageIndices = &target->current_index,
	};

#ifdef XRT_OS_WINDOWS
	// Tag this present with a VkPresentIdKHR when either consumer needs it:
	// the weave-latency harness (waiter thread timestamps scanout) or the
	// late-weave pacer (acquire waits on the previous id hitting glass).
	struct wl_harness *wl = wl_get(target);
	uint64_t wl_id = 0;
	VkPresentIdKHR present_id = {};
	if ((wl != nullptr || dxr_late_weave_enabled()) && target_present_wait_fn(target) != nullptr) {
		wl_id = ++target->present_id_counter;
		present_id.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
		present_id.swapchainCount = 1;
		present_id.pPresentIds = &wl_id;
		present_info.pNext = &present_id;
	}
#endif

	/*
	 * #902: tag the present so the driver RECORDS its timing.
	 *
	 * vkGetPastPresentationTimingGOOGLE only reports presents that carried a
	 * VkPresentTimesInfoGOOGLE — an untagged present is simply not recorded,
	 * which is why the feeder can read a valid refresh period and still never
	 * receive a single observation. That failure is silent and looks exactly
	 * like an idle panel.
	 *
	 * desiredPresentTime stays 0: we are asking for FEEDBACK, not scheduling.
	 * Supplying a target here would make the driver hold the frame, which is
	 * the opposite of what late weave wants.
	 *
	 * Mutually exclusive with the VkPresentIdKHR chain above by construction —
	 * that one only builds on a path where present_wait resolved, and this
	 * extension only matters where it did not.
	 */
	VkPresentTimeGOOGLE present_time = {};
	VkPresentTimesInfoGOOGLE present_times = {};
	if (vk->has_GOOGLE_display_timing && present_info.pNext == NULL) {
		present_time.presentID = ++target->display_timing_present_id;
		present_time.desiredPresentTime = 0;
		present_times.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE;
		present_times.swapchainCount = 1;
		present_times.pTimes = &present_time;
		present_info.pNext = &present_times;
	}

	VkResult res = vk->vkQueuePresentKHR(queue, &present_info);

	// #868/#902: anchor the vblank grid on what actually reached the panel.
	// After the present, so the driver has the record; best-effort, so a
	// failure here never affects the present's own result.
	if (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR) {
		target_feed_vblank_grid(target);
	}

#ifdef XRT_OS_WINDOWS
	if (wl_id != 0 && (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR)) {
		target->last_present_id = wl_id;
		target->last_present_weave_qpc = target->weave_mark_qpc;
		target->present_qpc_ring_id[wl_id % 8] = wl_id;
		target->present_qpc_ring[wl_id % 8] = target->weave_mark_qpc;
		if (wl != nullptr) {
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			{
				std::lock_guard<std::mutex> lock(wl->mtx);
				if (wl->pending_weave_qpc != 0) {
					// #1048: trailing repaint field, same F-row shape as the
					// D3D11/D3D12 sites (F,seq,qpc_weave,qpc_present_ret,
					// present_count,repaint) so the offline parser can count
					// the two present populations apart.
					fprintf(wl->f, "F,%llu,%llu,%llu,%llu,%d\n", (unsigned long long)wl_id,
					        (unsigned long long)wl->pending_weave_qpc,
					        (unsigned long long)now.QuadPart, (unsigned long long)wl_id,
					        wl->pending_repaint ? 1 : 0);
					wl->pending_weave_qpc = 0;
					wl->pending_repaint = false;
					wl->rows_written++;
				}
				wl->ids.push_back(wl_id);
			}
			wl->cv.notify_one();
		}
	}
#endif

	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
		return XRT_SUCCESS;
	}
	if (res != VK_SUCCESS) {
		U_LOG_E("Present failed: %d", res);
		return XRT_ERROR_VULKAN;
	}

	return XRT_SUCCESS;
}

void
comp_vk_native_target_get_dimensions(struct comp_vk_native_target *target,
                                      uint32_t *out_width,
                                      uint32_t *out_height)
{
	*out_width = target->width;
	*out_height = target->height;
}

void
comp_vk_native_target_get_current_image(struct comp_vk_native_target *target,
                                         uint64_t *out_image,
                                         uint64_t *out_view)
{
	*out_image = (uint64_t)(uintptr_t)target->images[target->current_index];
	*out_view = (uint64_t)(uintptr_t)target->views[target->current_index];
}

VkFormat
comp_vk_native_target_get_format(struct comp_vk_native_target *target)
{
	return target->format;
}

xrt_result_t
comp_vk_native_target_resize(struct comp_vk_native_target *target,
                               uint32_t width,
                               uint32_t height)
{
	struct vk_bundle *vk = target->vk;

	if (width == target->width && height == target->height) {
		return XRT_SUCCESS;
	}

#ifdef XRT_OS_WINDOWS
	// Transparent path: DComp bridge has no WSI surface/swapchain — the
	// dcomp DXGI swapchain and KMT-shared D3D11 textures (imported as
	// VkImages) are size-bound. Tear down the bridge and re-run
	// dcomp_setup at the new dimensions. target->views/images alias
	// dcomp_vk_view/image, and dcomp_destroy frees the underlying handles,
	// so we must NOT call destroy_swapchain_views here (would double-free)
	// and must NOT fall through to create_swapchain (target->surface is
	// VK_NULL_HANDLE in DComp mode → vkGetPhysicalDeviceSurfaceCapabilitiesKHR
	// would crash).
	if (target->dcomp_active) {
		if (target->hwnd == NULL) {
			U_LOG_E("DComp bridge resize: target->hwnd is NULL");
			return XRT_ERROR_VULKAN;
		}
		// Exclude the unlocked swapchain users for the whole rebuild: this
		// branch nulls target->views/images below, and the repaint replay
		// hands those straight to the vendor weaver.
		target_recreate_guard guard(target);
		vk->vkDeviceWaitIdle(vk->device);

		// IN-PLACE resize. Keep the D3D11 device + DComp device/target/visual
		// AND the composition swapchain object bound to the HWND — only the
		// size-bound resources change (ResizeBuffers on the swapchain + rebuild
		// the ring of KMT-shared textures / VkImage imports). The visual is
		// never unbound, so the window keeps showing content.
		//
		// REGRESSION FIX: the previous version called dcomp_destroy()+
		// dcomp_setup() here, which releases the visual/target (CreateTargetForHwnd
		// binding) and rebuilds the whole bridge. begin_frame polls GetClientRect
		// and calls this every frame the size changes, so during a live resize-drag
		// the visual was unbound/rebound every frame → the window was fully
		// transparent for the entire drag. ResizeBuffers preserves the binding.

		// 1. Release only the size-bound ring (VkImages/views/memory + shared D3D11).
		for (uint32_t i = 0; i < DCOMP_RING; i++) {
			if (target->dcomp_vk_view[i]) { vk->vkDestroyImageView(vk->device, target->dcomp_vk_view[i], NULL); target->dcomp_vk_view[i] = VK_NULL_HANDLE; }
			if (target->dcomp_vk_image[i]) { vk->vkDestroyImage(vk->device, target->dcomp_vk_image[i], NULL); target->dcomp_vk_image[i] = VK_NULL_HANDLE; }
			if (target->dcomp_vk_memory[i]) { vk->vkFreeMemory(vk->device, target->dcomp_vk_memory[i], NULL); target->dcomp_vk_memory[i] = VK_NULL_HANDLE; }
			if (target->dcomp_shared_mutex[i]) { target->dcomp_shared_mutex[i]->Release(); target->dcomp_shared_mutex[i] = NULL; }
			if (target->dcomp_shared_dx[i]) { target->dcomp_shared_dx[i]->Release(); target->dcomp_shared_dx[i] = NULL; }
			target->views[i] = VK_NULL_HANDLE;
			target->images[i] = VK_NULL_HANDLE;
		}
		target->image_count = 0;

		// 2. Resize the composition swapchain in place. The swapchain object (and
		// the visual->SetContent binding to it) survives ResizeBuffers; no back
		// buffer is held (dcomp_present releases it each frame) so this succeeds.
		// #870/#848 — ResizeBuffers must receive the creation flags or DXGI
		// fails it with E_INVALIDARG (the waitable flag is now among them).
		HRESULT hr = target->dcomp_swapchain->ResizeBuffers(
		    DCOMP_RING, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, target->dcomp_swapchain_flags);
		if (FAILED(hr)) {
			U_LOG_E("DComp bridge resize: ResizeBuffers(%ux%u) failed: 0x%08x", width, height, hr);
			return XRT_ERROR_VULKAN;
		}

		// 3. Rebuild the ring of NT-handle-shared textures + VkImage imports at
		// the new size (same sharing mode as dcomp_setup — see the comment
		// there for why NT and not legacy KMT).
		for (uint32_t i = 0; i < DCOMP_RING; i++) {
			D3D11_TEXTURE2D_DESC tdesc = {};
			tdesc.Width = width;
			tdesc.Height = height;
			tdesc.MipLevels = 1;
			tdesc.ArraySize = 1;
			tdesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			tdesc.SampleDesc.Count = 1;
			tdesc.Usage = D3D11_USAGE_DEFAULT;
			tdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			tdesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
			hr = target->dcomp_dx_device->CreateTexture2D(&tdesc, NULL, &target->dcomp_shared_dx[i]);
			if (FAILED(hr)) {
				U_LOG_E("DComp bridge resize: CreateTexture2D[%u] failed: 0x%08x", i, hr);
				return XRT_ERROR_VULKAN;
			}
			IDXGIResource1 *dxgi_res = NULL;
			hr = target->dcomp_shared_dx[i]->QueryInterface(__uuidof(IDXGIResource1), (void **)&dxgi_res);
			if (FAILED(hr) || dxgi_res == NULL) {
				U_LOG_E("DComp bridge resize: QueryInterface(IDXGIResource1)[%u] failed: 0x%08x", i, hr);
				return XRT_ERROR_VULKAN;
			}
			HANDLE shared_nt = NULL;
			hr = dxgi_res->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
			                                  NULL, &shared_nt);
			dxgi_res->Release();
			if (FAILED(hr) || shared_nt == NULL) {
				U_LOG_E("DComp bridge resize: CreateSharedHandle[%u] failed: 0x%08x", i, hr);
				return XRT_ERROR_VULKAN;
			}
			bool imported = dcomp_import_one(target, i, target->dcomp_shared_dx[i], shared_nt,
			                                  width, height, VK_FORMAT_B8G8R8A8_UNORM);
			CloseHandle(shared_nt);
			if (!imported) {
				U_LOG_E("DComp bridge resize: import ring[%u] failed", i);
				return XRT_ERROR_VULKAN;
			}
		}

		// 4. Republish the new images/views + dims. Visual/target/device untouched.
		target->image_count = DCOMP_RING;
		for (uint32_t i = 0; i < DCOMP_RING; i++) {
			target->images[i] = target->dcomp_vk_image[i];
			target->views[i] = target->dcomp_vk_view[i];
		}
		target->format = VK_FORMAT_B8G8R8A8_UNORM;
		target->width = width;
		target->height = height;
		target->dcomp_ring_idx = 0;
		target->current_index = 0;
		target->generation++; // #602: image set rebuilt — invalidate DP caches.
		if (target->dcomp_dcomp_device != NULL) { target->dcomp_dcomp_device->Commit(); }
		return XRT_SUCCESS;
	}
#endif

#ifdef XRT_OS_WINDOWS
	// Present ids are per-swapchain: drop the harness (it lazily re-arms on
	// the new swapchain) and reset the id sequence, exactly as the out-of-date
	// recreate in target_acquire does. MUST precede the guard — wl_teardown
	// joins a thread that takes swapchain_mutex.
	wl_teardown(target);
	target->present_id_counter = 0;
	target->last_present_id = 0;
#endif

	// #902: the grid's PHASE is meaningless across a new swapchain, but
	// the panel's period usually is not — reset_phase keeps the period so
	// the grid re-anchors on the first present instead of re-probing.
	// refresh_cycle_probed clears too: a recreate can follow a mode change.
	comp_vblank_grid_reset_phase(&target->vblank_grid);
	target->refresh_cycle_probed = false;

	// The crash this exists to stop: below, target->swapchain is set to
	// VK_NULL_HANDLE before create_swapchain repopulates it, and the repaint
	// thread reads it unlocked in comp_vk_native_target_repaint_pace.
	target_recreate_guard guard(target);

	vk->vkDeviceWaitIdle(vk->device);

	destroy_swapchain_views(target);

	// Destroy old swapchain BEFORE creating new one — only one active
	// swapchain per surface is allowed (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR).
	// This matches the destroy-before-create pattern in target_acquire.
	if (target->swapchain != VK_NULL_HANDLE) {
		vk->vkDestroySwapchainKHR(vk->device, target->swapchain, NULL);
		target->swapchain = VK_NULL_HANDLE;
	}

	target->width = width;
	target->height = height;

	return create_swapchain(target); // bumps target->generation on success (#602)
}

uint32_t
comp_vk_native_target_get_generation(struct comp_vk_native_target *target)
{
	return target->generation;
}
