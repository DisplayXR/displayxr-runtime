// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  weave_present_vk_linux — a desktop-Linux XR_DXR_weave PRESENT-OWNER
 *         with a real window: what a browser's GPU process does, in miniature
 *         (#1699).
 *
 * The app owns its window AND its swapchain. The runtime owns neither: it is a
 * weave SERVICE in another process (displayxr-service, reached over IPC) that
 * receives the app's pre-weave stereo pixels as a dma-buf, weaves them with the
 * display processor, and hands the woven pixels back as a dma-buf for the app
 * to present. Per frame:
 *
 *   1. RENDER. The app's own Vulkan device ray-casts a stereo test scene into
 *      an offscreen image — a squeezed side-by-side pair, left eye in the left
 *      half (scene.frag: a rotating cube, a checkered back wall, a colour-bar
 *      strip at the display plane, a 1-px border per half) — adds the labels,
 *      and copies it into one of two exportable, window-sized dma-bufs
 *      (driver-picked DRM modifier). The HUD, when it changes, goes into a
 *      window-sized premultiplied overlay dma-buf. Both are released to
 *      VK_QUEUE_FAMILY_FOREIGN_EXT and the submit signals an exportable
 *      SYNC_FD semaphore — the ACQUIRE fence. No CPU wait.
 *   2. SUBMIT. xrWeaveSubmitDXR, spec v10: XrWeaveSubmitRectsDXR (one rect,
 *      the whole window), XrWeaveSubmitOverlaysDXR + its
 *      XrWeaveOverlayDmabufDescDXR (the HUD, composited 2D OVER the weave by
 *      the display processor), XrWeaveDmabufDescDXR (the input, stable
 *      bufferId per pool slot), XrWeaveSubmitSyncDXR (the acquire fence),
 *      firstChunk. Out: XrWeaveOutputDmabufDXR (the woven dma-buf, on the
 *      first frame and every reallocation) and XrWeaveOutputSyncDXR (a
 *      RELEASE sync_file, every frame), plus the tracked eyes, which drive the
 *      NEXT frame's projection.
 *   3. PRESENT. The release fence is imported into a semaphore and waited ON
 *      THE GPU by the submit that copies the woven dma-buf into the acquired
 *      swapchain image; then vkQueuePresentKHR. The service never touches the
 *      window.
 *   4. GEOMETRY. The window's client rect in DESKTOP-ABSOLUTE DEVICE PIXELS is
 *      re-bound (xrWeaveBindWindow2DXR + XrWeaveWindowGeometryDXR) whenever it
 *      changes; the service turns it into the panel-relative present origin
 *      the display processor phases the interlace to. X11:
 * XTranslateCoordinates. Wayland: the window-geometry@displayxr.org GNOME Shell
 * extension's GetWindows, converted with u_wayland_geom.h (logical x fractional
 *      scale, monitor-relative, plus the runtime's own panel origin).
 *   5. DRAG. The window helper's drag goes through the display processor's
 *      snap (DxrWeaveSnap -> xrWeaveSnapWindowRectDXR): every step on X11, and
 *      on Wayland the compositor-run drag lattice (SetDragLattice). After a
 *      Wayland drop the app snaps once more and asks the extension to
 *      MoveWindow — the job the runtime's own compositor does for an
 *      in-process app, and which a present-owner must do itself.
 *
 * Modes:
 *   (default)          present the woven output
 *   --sbs              present the UNWOVEN side-by-side input (what the
 *                      pattern looks like without a weave); the weave still
 * runs
 *   --anaglyph-check   poll $TMPDIR/weave_present_trigger; when it appears,
 * dump the woven output and the SBS input as PNGs next to it and log how well
 * the woven frame matches a red/cyan anaglyph of the input (sim_display's
 * default weave)
 *   --headless N       NO window: an offscreen 1280x720 target, N frames of the
 *                      same submit/present pipeline, then a self-check (the
 *                      presented frame == the woven output, or == the SBS input
 *                      with --sbs; woven non-black and != SBS; HUD composited;
 *                      fd counts flat; with --expect-anaglyph, woven ==
 *                      anaglyph(SBS)) and PNGs of the woven output, the SBS
 *                      input and the presented frame. Exit code 0 = PASS.
 *                      --test-resize=WxH@N reallocates mid-run, as a window
 *                      resize or F11 does.
 *   --lattice-selftest one Wayland drag-lattice table build per output scale
 *                      (100 %, 200 %, 150 %) through the helper's own table
 *                      code (dxr_wl_lattice::probe_via_grid) and the SAME
 *                      providers the window installs (DxrWeaveSnap::callback +
 *                      grid_callback), against the per-point build: logs the
 *                      IPC call count and ms of each, fails unless the tables
 *                      are identical and 100 %/200 % take <= 3 calls. Needs no
 *                      window (pair it with --headless N).
 *
 * Needs a running displayxr-service (the run script forces XRT_FORCE_MODE=ipc):
 * a weave present-owner exists only on the service path.
 */

// Xlib (and, in a Wayland-capable build, wayland-client) before anything that
// names Display / wl_surface.
#include "dxr_linux_window.h"
#include "dxr_weave_snap.h"
#include "dxr_wl_lattice.h" // --lattice-selftest: the helper's pure table build

#include <X11/Xlib.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xlib.h>
#ifdef DXR_APP_HAVE_WAYLAND
#include <vulkan/vulkan_wayland.h>
#endif

#define XR_USE_GRAPHICS_API_VULKAN 1
#include <openxr/XR_DXR_display_info.h>
#include <openxr/XR_DXR_weave.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "shaders/scene_spv.h"
#include "text5x7.h"
#include "weave_dmabuf_vk.h"
#include "wl_geometry_client.h"

#include "u_wayland_geom.h"
#include "u_x11_scale.h"

#include "stb_image_write.h"

#include <X11/keysym.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(__linux__) || defined(__ANDROID__)
#error "weave_present_vk_linux is a desktop-Linux app (XRT_OS_LINUX_DESKTOP)"
#endif

/*
 *
 * Logging.
 *
 */

static void
logf(const char *level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "[weave_present] %s", level);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}
#define LOGI(...) logf("", __VA_ARGS__)
#define LOGW(...) logf("WARN: ", __VA_ARGS__)
#define LOGE(...) logf("ERROR: ", __VA_ARGS__)

#define VK_OK(call)                                                                                                    \
	do {                                                                                                           \
		VkResult _v = (call);                                                                                  \
		if (_v != VK_SUCCESS) {                                                                                \
			LOGE("%s -> %d (%s:%d)", #call, (int)_v, __FILE__, __LINE__);                                  \
			return false;                                                                                  \
		}                                                                                                      \
	} while (0)

#define XR_OK(call)                                                                                                    \
	do {                                                                                                           \
		XrResult _r = (call);                                                                                  \
		if (XR_FAILED(_r)) {                                                                                   \
			LOGE("%s -> %d", #call, (int)_r);                                                              \
			return false;                                                                                  \
		}                                                                                                      \
	} while (0)

static volatile sig_atomic_t g_quit = 0;
static void
on_signal(int)
{
	g_quit = 1;
}

static int64_t
now_ns()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/*
 *
 * Options.
 *
 */

struct Options
{
	DxrWindowBackend backend = DxrWindowBackend::Auto;
	uint32_t width = 1280, height = 720; //!< requested content size, DEVICE px
	                                     //!< (the window helper's unit)
	bool fullscreen = false;
	bool sbs = false;
	bool anaglyph_check = false;
	bool hud = true;
	bool force_linear = false;
	int headless_frames = 0; //!< > 0: --headless N
	bool expect_anaglyph = false;
	long service_pid = 0;
	std::string dump_dir;
	//! (headless) --test-resize=WxH@N: reallocate everything at frame N, as a
	//! window resize / F11 does — the service must hand back a new output.
	uint32_t resize_w = 0, resize_h = 0;
	int resize_frame = -1;
	bool lattice_selftest = false; //!< --lattice-selftest
};

static void
usage(const char *argv0)
{
	fprintf(stderr,
	        "usage: %s [options]\n"
	        "  --platform=x11|wayland|auto  window backend (default auto: native "
	        "Wayland when ready)\n"
	        "  --size=WxH                   window content size in DEVICE px "
	        "(default 1280x720;\n"
	        "                               2560x1440 is 1280x720 logical on a "
	        "200%% output)\n"
	        "  --fullscreen                 start fullscreen on the 3D panel "
	        "(F11 toggles either way)\n"
	        "  --sbs                        present the UNWOVEN side-by-side "
	        "input instead of the weave\n"
	        "  --anaglyph-check             dump woven + SBS PNGs when "
	        "$TMPDIR/weave_present_trigger appears\n"
	        "  --no-hud                     submit no 2D overlay\n"
	        "  --linear                     LINEAR dma-buf inputs instead of the "
	        "driver's modifier\n"
	        "  --headless N                 no window: N offscreen frames + a "
	        "self-check (exit 0 = PASS)\n"
	        "  --expect-anaglyph            (headless) also require woven == "
	        "anaglyph(SBS)\n"
	        "  --service-pid=PID            (headless) also require the "
	        "service's fd count to stay flat\n"
	        "  --test-resize=WxH@N          (headless) reallocate at frame N, "
	        "like a window resize\n"
	        "  --lattice-selftest           build one drag-lattice table per "
	        "scale via the grid provider (exit 1 = FAIL)\n"
	        "  --dump-dir=DIR               where PNGs go (default $TMPDIR, else "
	        "/tmp)\n"
	        "Keys: ESC quits, F11 toggles fullscreen, S toggles woven/SBS "
	        "presentation.\n",
	        argv0);
}

static bool
parse_args(int argc, char **argv, Options &o)
{
	std::string err;
	if (const char *benv = getenv("DXR_WINDOW_BACKEND")) {
		DxrLinuxWindow::parse_backend(benv, &o.backend);
	}
	if (!DxrLinuxWindow::parse_platform_args(argc, argv, &o.backend, &err)) {
		LOGE("%s", err.c_str());
		return false;
	}
	if (const char *t = getenv("TMPDIR")) {
		o.dump_dir = t;
	} else {
		o.dump_dir = "/tmp";
	}
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (strncmp(a, "--platform", 10) == 0 || strncmp(a, "--backend", 9) == 0) {
			if (strchr(a, '=') == nullptr) {
				i++; // "--platform x11": value consumed by parse_platform_args
			}
		} else if (strncmp(a, "--frame-stats", 13) == 0) {
			// handled by the window helper
		} else if (sscanf(a, "--size=%ux%u", &o.width, &o.height) == 2) {
		} else if (strcmp(a, "--fullscreen") == 0) {
			o.fullscreen = true;
		} else if (strcmp(a, "--sbs") == 0) {
			o.sbs = true;
		} else if (strcmp(a, "--anaglyph-check") == 0) {
			o.anaglyph_check = true;
		} else if (strcmp(a, "--no-hud") == 0) {
			o.hud = false;
		} else if (strcmp(a, "--linear") == 0) {
			o.force_linear = true;
		} else if (strcmp(a, "--headless") == 0 && i + 1 < argc) {
			o.headless_frames = atoi(argv[++i]);
		} else if (strncmp(a, "--headless=", 11) == 0) {
			o.headless_frames = atoi(a + 11);
		} else if (strcmp(a, "--expect-anaglyph") == 0) {
			o.expect_anaglyph = true;
		} else if (strncmp(a, "--service-pid=", 14) == 0) {
			o.service_pid = atol(a + 14);
		} else if (sscanf(a, "--test-resize=%ux%u@%d", &o.resize_w, &o.resize_h, &o.resize_frame) == 3) {
		} else if (strcmp(a, "--lattice-selftest") == 0) {
			o.lattice_selftest = true;
		} else if (strncmp(a, "--dump-dir=", 11) == 0) {
			o.dump_dir = a + 11;
		} else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
			usage(argv[0]);
			exit(0);
		} else {
			LOGE("unknown argument \"%s\"", a);
			usage(argv[0]);
			return false;
		}
	}
	if (o.width < 64 || o.height < 64) {
		LOGE("--size too small");
		return false;
	}
	if (o.headless_frames < 0) {
		o.headless_frames = 0;
	}
	return true;
}

/*
 *
 * State.
 *
 */

struct HostBuffer
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	uint8_t *map = nullptr;
	VkDeviceSize size = 0;
};

struct LabelRegion
{
	int x, y, w, h;
	VkDeviceSize offset;
};

struct App
{
	Options opt;
	bool headless = false;

	// ---- OpenXR
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	PFN_xrWeaveBindWindow2DXR pfn_bind2 = nullptr;
	PFN_xrWeaveSubmitDXR pfn_submit = nullptr;
	PFN_xrWeaveSnapWindowRectDXR pfn_snap = nullptr;
	bool has_display_info = false;
	int32_t panel_left = 0, panel_top = 0;
	uint32_t panel_w = 0, panel_h = 0;
	float panel_w_m = 0.344f,
	      panel_h_m = 0.194f; //!< sim_display's defaults until display_info says otherwise

	// ---- Window
	DxrLinuxWindow window;
	DxrWeaveSnap weave_snap;
	WlGeometryClient wl_geom;
	bool wl_geom_ok = false;

	// ---- Vulkan
	VkInstance vk_instance = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t qfi = 0;
	WeaveDmabufDevice dd;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer render_cmd = VK_NULL_HANDLE, present_cmd = VK_NULL_HANDLE;
	VkFence render_fence = VK_NULL_HANDLE, present_fence = VK_NULL_HANDLE;
	bool render_pending = false, present_pending = false;
	VkSemaphore acq_sem = VK_NULL_HANDLE; //!< exportable SYNC_FD: the acquire fence
	VkSemaphore rel_sem = VK_NULL_HANDLE; //!< the service's release sync_file is imported here
	VkRenderPass render_pass = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;

	// ---- Presentation
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat sc_format = VK_FORMAT_UNDEFINED;
	std::vector<VkImage> sc_images;
	std::vector<VkSemaphore> sc_done; //!< per image: present waits it
	VkSemaphore img_avail = VK_NULL_HANDLE;
	bool swapchain_dirty = false;
	// headless "present" target
	VkImage target = VK_NULL_HANDLE;
	VkDeviceMemory target_mem = VK_NULL_HANDLE;

	// ---- Size-dependent content
	uint32_t w = 0, h = 0; //!< content = window client, DEVICE px
	uint32_t generation = 0;
	VkImage scene = VK_NULL_HANDLE;
	VkDeviceMemory scene_mem = VK_NULL_HANDLE;
	VkImageView scene_view = VK_NULL_HANDLE;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	HostBuffer labels;
	std::vector<LabelRegion> label_regions;
	DmabufImage input[2];
	DmabufImage overlay;
	HostBuffer overlay_staging;
	bool overlay_full_upload = true;
	bool overlay_dirty = true;
	int hud_x = 0, hud_y = 0, hud_w = 0, hud_h = 0;
	HostBuffer rb_woven, rb_sbs,
	    rb_presented; //!< readbacks (headless / dump trigger)

	// ---- Woven output (the service's)
	DmabufImage output;
	uint64_t out_modifier = 0;

	// ---- Per-frame state
	uint64_t frame = 0;
	XrVector3f eyes[2] = {{-0.032f, 0.0f, 0.6f}, {0.032f, 0.0f, 0.6f}};
	bool eyes_valid = false, eyes_tracking = false;
	bool have_geom = false;
	int32_t geom_x = 0,
	        geom_y = 0; //!< bound client origin, desktop-absolute DEVICE px
	uint32_t geom_w = 0, geom_h = 0;
	const char *geom_source = "none";
	bool warned_no_geom = false;
	double fps = 0.0;
	int64_t fps_t0 = 0;
	uint64_t fps_frames = 0;
	int64_t hud_t = 0;
	bool dump_requested = false;
	int submit_failures = 0;

	// ---- Wayland drop snap (see wl_drop_snap)
	struct
	{
		bool have_last = false, moving = false, verify = false;
		int32_t last_x = 0, last_y = 0, anchor_x = 0, anchor_y = 0, want_x = 0, want_y = 0;
		int still = 0, verify_polls = 0, tries = 0;
		bool warned_scale = false;
	} ds;
};

static App g;

/*
 *
 * Small Vulkan helpers.
 *
 */

static bool
create_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, HostBuffer &out)
{
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = size;
	bci.usage = usage;
	VK_OK(vkCreateBuffer(g.device, &bci, nullptr, &out.buffer));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(g.device, out.buffer, &req);
	uint32_t type = 0;
	if (!weave_find_memory_type(g.dd, req.memoryTypeBits,
	                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                            &type)) {
		LOGE("no host-visible memory type");
		return false;
	}
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = type;
	VK_OK(vkAllocateMemory(g.device, &mai, nullptr, &out.memory));
	VK_OK(vkBindBufferMemory(g.device, out.buffer, out.memory, 0));
	void *p = nullptr;
	VK_OK(vkMapMemory(g.device, out.memory, 0, VK_WHOLE_SIZE, 0, &p));
	out.map = (uint8_t *)p;
	out.size = size;
	return true;
}

static void
destroy_host_buffer(HostBuffer &b)
{
	if (b.buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(g.device, b.buffer, nullptr);
	}
	if (b.memory != VK_NULL_HANDLE) {
		vkFreeMemory(g.device, b.memory, nullptr);
	}
	b = HostBuffer{};
}

static bool
create_device_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImage *img, VkDeviceMemory *mem)
{
	VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = fmt;
	ici.extent = {w, h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = usage;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_OK(vkCreateImage(g.device, &ici, nullptr, img));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(g.device, *img, &req);
	uint32_t type = 0;
	if (!weave_find_memory_type(g.dd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
		return false;
	}
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = type;
	VK_OK(vkAllocateMemory(g.device, &mai, nullptr, mem));
	VK_OK(vkBindImageMemory(g.device, *img, *mem, 0));
	return true;
}

static void
image_barrier(VkCommandBuffer cmd,
              VkImage image,
              VkImageLayout old_l,
              VkImageLayout new_l,
              VkAccessFlags src_a,
              VkAccessFlags dst_a,
              VkPipelineStageFlags src_s,
              VkPipelineStageFlags dst_s)
{
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = src_a;
	b.dstAccessMask = dst_a;
	b.oldLayout = old_l;
	b.newLayout = new_l;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, src_s, dst_s, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static bool
begin_cmd(VkCommandBuffer cmd)
{
	VK_OK(vkResetCommandBuffer(cmd, 0));
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_OK(vkBeginCommandBuffer(cmd, &bi));
	return true;
}

static bool
wait_frame_fences()
{
	const uint64_t kTimeout = 5ULL * 1000 * 1000 * 1000;
	if (g.render_pending) {
		VK_OK(vkWaitForFences(g.device, 1, &g.render_fence, VK_TRUE, kTimeout));
		VK_OK(vkResetFences(g.device, 1, &g.render_fence));
		g.render_pending = false;
	}
	if (g.present_pending) {
		VK_OK(vkWaitForFences(g.device, 1, &g.present_fence, VK_TRUE, kTimeout));
		VK_OK(vkResetFences(g.device, 1, &g.present_fence));
		g.present_pending = false;
	}
	return true;
}

/*
 *
 * OpenXR + Vulkan bring-up (XR_KHR_vulkan_enable2: the runtime creates the
 * VkInstance / VkDevice with our extensions appended).
 *
 */

static bool
init_openxr()
{
	uint32_t n = 0;
	XR_OK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr));
	std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
	XR_OK(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data()));
	bool has_weave = false, has_vk2 = false;
	for (const auto &e : exts) {
		has_weave = has_weave || strcmp(e.extensionName, XR_DXR_WEAVE_EXTENSION_NAME) == 0;
		has_vk2 = has_vk2 || strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0;
		g.has_display_info =
		    g.has_display_info || strcmp(e.extensionName, XR_DXR_DISPLAY_INFO_EXTENSION_NAME) == 0;
	}
	LOGI("XR_DXR_weave %s, XR_KHR_vulkan_enable2 %s, XR_DXR_display_info %s", has_weave ? "AVAILABLE" : "MISSING",
	     has_vk2 ? "AVAILABLE" : "MISSING", g.has_display_info ? "AVAILABLE" : "MISSING");
	if (!has_weave || !has_vk2) {
		LOGE("this runtime cannot serve a Vulkan weave present-owner");
		return false;
	}
	// No window-binding extension on purpose: the app presents, the runtime
	// never sees the window. XR_DXR_weave makes this client a PRESENT_OWNER.
	std::vector<const char *> enabled = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_DXR_WEAVE_EXTENSION_NAME};
	if (g.has_display_info) {
		enabled.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);
	}
	XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
	snprintf(ici.applicationInfo.applicationName, sizeof(ici.applicationInfo.applicationName), "%s",
	         "WeavePresentVkLinux");
	ici.applicationInfo.applicationVersion = 1;
	snprintf(ici.applicationInfo.engineName, sizeof(ici.applicationInfo.engineName), "%s", "None");
	ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	ici.enabledExtensionCount = (uint32_t)enabled.size();
	ici.enabledExtensionNames = enabled.data();
	XR_OK(xrCreateInstance(&ici, &g.instance));

	XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_OK(xrGetSystem(g.instance, &sgi, &g.system));

	if (g.has_display_info) {
		XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
		XrDisplayInfoDXR di = {XR_TYPE_DISPLAY_INFO_DXR};
		XrDisplayDesktopPositionDXR pos = {};
		pos.type = XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR;
		di.next = &pos;
		sp.next = &di;
		if (XR_SUCCEEDED(xrGetSystemProperties(g.instance, g.system, &sp))) {
			g.panel_left = pos.left;
			g.panel_top = pos.top;
			g.panel_w = di.displayPixelWidth;
			g.panel_h = di.displayPixelHeight;
			if (di.displaySizeMeters.width > 0.0f && di.displaySizeMeters.height > 0.0f) {
				g.panel_w_m = di.displaySizeMeters.width;
				g.panel_h_m = di.displaySizeMeters.height;
			}
		}
	}
	if (g.panel_w == 0 || g.panel_h == 0) {
		g.panel_w = 1920;
		g.panel_h = 1080;
		LOGW(
		    "no panel size from XR_DXR_display_info — assuming 1920x1080 at the "
		    "desktop origin");
	}
	LOGI("3D panel: %ux%u px at desktop (%d, %d), %.4f x %.4f m", g.panel_w, g.panel_h, g.panel_left, g.panel_top,
	     (double)g.panel_w_m, (double)g.panel_h_m);
	return true;
}

static bool
device_has_ext(const std::vector<VkExtensionProperties> &have, const char *name)
{
	for (const auto &e : have) {
		if (strcmp(e.extensionName, name) == 0) {
			return true;
		}
	}
	return false;
}

static bool
init_vulkan()
{
	PFN_xrGetVulkanGraphicsRequirements2KHR pfn_req = nullptr;
	PFN_xrCreateVulkanInstanceKHR pfn_inst = nullptr;
	PFN_xrGetVulkanGraphicsDevice2KHR pfn_gdev = nullptr;
	PFN_xrCreateVulkanDeviceKHR pfn_dev = nullptr;
	xrGetInstanceProcAddr(g.instance, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction *)&pfn_req);
	xrGetInstanceProcAddr(g.instance, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction *)&pfn_inst);
	xrGetInstanceProcAddr(g.instance, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction *)&pfn_gdev);
	xrGetInstanceProcAddr(g.instance, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction *)&pfn_dev);
	if (!pfn_req || !pfn_inst || !pfn_gdev || !pfn_dev) {
		LOGE("XR_KHR_vulkan_enable2 entry points missing");
		return false;
	}
	XrGraphicsRequirementsVulkan2KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
	XR_OK(pfn_req(g.instance, g.system, &req));

	std::vector<const char *> iexts;
	if (!g.headless) {
		iexts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef DXR_APP_HAVE_WAYLAND
		if (g.window.backend() == DxrWindowBackend::Wayland) {
			iexts.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
		} else
#endif
		{
			iexts.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
		}
	}
	VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	ai.pApplicationName = "WeavePresentVkLinux";
	ai.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo vici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	vici.pApplicationInfo = &ai;
	vici.enabledExtensionCount = (uint32_t)iexts.size();
	vici.ppEnabledExtensionNames = iexts.data();
	XrVulkanInstanceCreateInfoKHR xvici = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
	xvici.systemId = g.system;
	xvici.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xvici.vulkanCreateInfo = &vici;
	VkResult vr = VK_SUCCESS;
	XR_OK(pfn_inst(g.instance, &xvici, &g.vk_instance, &vr));
	VK_OK(vr);

	XrVulkanGraphicsDeviceGetInfoKHR gdi = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
	gdi.systemId = g.system;
	gdi.vulkanInstance = g.vk_instance;
	XR_OK(pfn_gdev(g.instance, &gdi, &g.phys));
	{
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(g.phys, &p);
		LOGI("GPU: %s", p.deviceName);
	}

	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, nullptr);
	std::vector<VkQueueFamilyProperties> qfs(qn);
	vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, qfs.data());
	g.qfi = UINT32_MAX;
	for (uint32_t i = 0; i < qn; i++) {
		if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			g.qfi = i;
			break;
		}
	}
	if (g.qfi == UINT32_MAX) {
		LOGE("no graphics queue family");
		return false;
	}

	uint32_t en = 0;
	vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &en, nullptr);
	std::vector<VkExtensionProperties> have(en);
	vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &en, have.data());
	std::vector<const char *> dexts;
	for (const char *e : kWeaveDmabufDeviceExtensions) {
		if (!device_has_ext(have, e)) {
			LOGE("device lacks %s — cannot share dma-bufs with the weave service", e);
			return false;
		}
		dexts.push_back(e);
	}
	if (!g.headless) {
		dexts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	}
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = g.qfi;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = (uint32_t)dexts.size();
	dci.ppEnabledExtensionNames = dexts.data();
	XrVulkanDeviceCreateInfoKHR xdci = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
	xdci.systemId = g.system;
	xdci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xdci.vulkanPhysicalDevice = g.phys;
	xdci.vulkanCreateInfo = &dci;
	XR_OK(pfn_dev(g.instance, &xdci, &g.device, &vr));
	VK_OK(vr);
	vkGetDeviceQueue(g.device, g.qfi, 0, &g.queue);
	if (!g.dd.load(g.phys, g.device)) {
		LOGE("dma-buf / sync_fd entry points unavailable");
		return false;
	}

	VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = g.qfi;
	VK_OK(vkCreateCommandPool(g.device, &pci, nullptr, &g.pool));
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = g.pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VK_OK(vkAllocateCommandBuffers(g.device, &cai, &g.render_cmd));
	VK_OK(vkAllocateCommandBuffers(g.device, &cai, &g.present_cmd));
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VK_OK(vkCreateFence(g.device, &fci, nullptr, &g.render_fence));
	VK_OK(vkCreateFence(g.device, &fci, nullptr, &g.present_fence));
	if (!weave_create_sync_fd_semaphore(g.device, true, &g.acq_sem) ||
	    !weave_create_sync_fd_semaphore(g.device, false, &g.rel_sem)) {
		return false;
	}
	VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VK_OK(vkCreateSemaphore(g.device, &sci, nullptr, &g.img_avail));
	return true;
}

static bool
init_session()
{
	XrGraphicsBindingVulkan2KHR binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
	binding.instance = g.vk_instance;
	binding.physicalDevice = g.phys;
	binding.device = g.device;
	binding.queueFamilyIndex = g.qfi;
	binding.queueIndex = 0;
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &binding;
	sci.systemId = g.system;
	XR_OK(xrCreateSession(g.instance, &sci, &g.session));
	xrGetInstanceProcAddr(g.instance, "xrWeaveBindWindow2DXR", (PFN_xrVoidFunction *)&g.pfn_bind2);
	xrGetInstanceProcAddr(g.instance, "xrWeaveSubmitDXR", (PFN_xrVoidFunction *)&g.pfn_submit);
	xrGetInstanceProcAddr(g.instance, "xrWeaveSnapWindowRectDXR", (PFN_xrVoidFunction *)&g.pfn_snap);
	if (!g.pfn_bind2 || !g.pfn_submit || !g.pfn_snap) {
		LOGE("weave entry points missing");
		return false;
	}
	LOGI("session created (IPC present-owner; the runtime never sees the window)");
	return true;
}

/*
 *
 * The scene renderer: one fullscreen triangle, the rest is scene.frag.
 *
 */

struct ScenePush
{
	float eye_l[4];
	float eye_r[4];
	float canvas[4];
	float cube[4];
	float misc[4];
};
static_assert(sizeof(ScenePush) == 80, "push constant block");

static bool
init_pipeline()
{
	VkAttachmentDescription att = {};
	att.format = VK_FORMAT_B8G8R8A8_UNORM;
	att.samples = VK_SAMPLE_COUNT_1_BIT;
	att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // the shader writes every pixel
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; // labels are copied in next
	VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub = {};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &ref;
	VkSubpassDependency deps[2] = {};
	deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	deps[0].dstSubpass = 0;
	deps[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
	deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	deps[1].srcSubpass = 0;
	deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
	deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	deps[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	rpci.attachmentCount = 1;
	rpci.pAttachments = &att;
	rpci.subpassCount = 1;
	rpci.pSubpasses = &sub;
	rpci.dependencyCount = 2;
	rpci.pDependencies = deps;
	VK_OK(vkCreateRenderPass(g.device, &rpci, nullptr, &g.render_pass));

	VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScenePush)};
	VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	VK_OK(vkCreatePipelineLayout(g.device, &plci, nullptr, &g.pipeline_layout));

	VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
	VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	smci.codeSize = sizeof(g_scene_vert_spv);
	smci.pCode = g_scene_vert_spv;
	VK_OK(vkCreateShaderModule(g.device, &smci, nullptr, &vs));
	smci.codeSize = sizeof(g_scene_frag_spv);
	smci.pCode = g_scene_frag_spv;
	VK_OK(vkCreateShaderModule(g.device, &smci, nullptr, &fs));
	VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
	                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vs;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fs;
	stages[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vps.viewportCount = 1;
	vps.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState cba = {};
	cba.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;
	VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dyn;
	VkGraphicsPipelineCreateInfo gpci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gpci.stageCount = 2;
	gpci.pStages = stages;
	gpci.pVertexInputState = &vi;
	gpci.pInputAssemblyState = &ia;
	gpci.pViewportState = &vps;
	gpci.pRasterizationState = &rs;
	gpci.pMultisampleState = &ms;
	gpci.pColorBlendState = &cb;
	gpci.pDynamicState = &ds;
	gpci.layout = g.pipeline_layout;
	gpci.renderPass = g.render_pass;
	VkResult r = vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &g.pipeline);
	vkDestroyShaderModule(g.device, vs, nullptr);
	vkDestroyShaderModule(g.device, fs, nullptr);
	VK_OK(r);
	return true;
}

/*
 *
 * Size-dependent content: scene target, labels, the dma-buf pool, the overlay.
 *
 */

//! Scale of the labels / HUD font for a content height.
static int
font_scale(uint32_t h)
{
	return std::max(1, (int)h / 360);
}

/*!
 * The scene labels, pre-squeezed (x scale = y scale / 2) because they live in
 * a squeezed side-by-side half: "LEFT EYE" / "RIGHT EYE" differ per half (so
 * each eye must read its own), the bar names are identical in both (screen
 * depth). Built once per size into one staging buffer; copied every frame.
 */
static bool
build_labels()
{
	destroy_host_buffer(g.labels);
	g.label_regions.clear();
	const int sy = 2 * font_scale(g.h);
	const int sx = std::max(1, sy / 2);
	const int ew = (int)g.w / 2;
	struct Pending
	{
		int x, y, w, h;
		std::string text;
	};
	std::vector<Pending> todo;
	const int pad = 2 * sx;
	const int lh = text5x7::kHeight * sy + 2 * pad;
	for (int eye = 0; eye < 2; eye++) {
		const char *t = eye == 0 ? "LEFT EYE" : "RIGHT EYE";
		const int lw = text5x7::width(t, sx) + 2 * pad;
		todo.push_back({eye * ew + 4 * sx, 4 * sy, lw, lh, t});
	}
	// Bar names, centred in each bar near the top of the strip (bars cover
	// the bottom 12% — scene.frag misc.w).
	static const char *kBarNames[8] = {"W", "Y", "C", "G", "M", "R", "B", "K"};
	const int strip_top = (int)((1.0f - 0.12f) * (float)g.h);
	for (int eye = 0; eye < 2; eye++) {
		for (int b = 0; b < 8; b++) {
			const int lw = text5x7::width(kBarNames[b], sx) + 2 * pad;
			const int cx = eye * ew + (int)((b + 0.5f) * (float)ew / 8.0f);
			todo.push_back({cx - lw / 2, strip_top + 2 * sy, lw, lh, kBarNames[b]});
		}
	}
	VkDeviceSize total = 0;
	for (const auto &p : todo) {
		total += (VkDeviceSize)p.w * p.h * 4;
	}
	if (!create_host_buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, g.labels)) {
		return false;
	}
	VkDeviceSize off = 0;
	for (const auto &p : todo) {
		if (p.x < 0 || p.y < 0 || p.x + p.w > (int)g.w || p.y + p.h > (int)g.h) {
			continue; // window too small for this label
		}
		uint8_t *dst = g.labels.map + off;
		text5x7::fill(dst, p.w, p.h, 0, 0, p.w, p.h, 0, 0, 0, 255);
		text5x7::draw(dst, p.w, p.h, pad, pad, p.text.c_str(), sx, sy, 255, 255, 255, 255);
		g.label_regions.push_back({p.x, p.y, p.w, p.h, off});
		off += (VkDeviceSize)p.w * p.h * 4;
	}
	return true;
}

static void
destroy_content()
{
	if (g.device == VK_NULL_HANDLE) {
		return;
	}
	vkDeviceWaitIdle(g.device);
	if (g.framebuffer) {
		vkDestroyFramebuffer(g.device, g.framebuffer, nullptr);
	}
	if (g.scene_view) {
		vkDestroyImageView(g.device, g.scene_view, nullptr);
	}
	if (g.scene) {
		vkDestroyImage(g.device, g.scene, nullptr);
	}
	if (g.scene_mem) {
		vkFreeMemory(g.device, g.scene_mem, nullptr);
	}
	g.framebuffer = VK_NULL_HANDLE;
	g.scene_view = VK_NULL_HANDLE;
	g.scene = VK_NULL_HANDLE;
	g.scene_mem = VK_NULL_HANDLE;
	destroy_host_buffer(g.labels);
	for (auto &in : g.input) {
		weave_destroy_dmabuf_image(g.device, in);
	}
	weave_destroy_dmabuf_image(g.device, g.overlay);
	destroy_host_buffer(g.overlay_staging);
	destroy_host_buffer(g.rb_woven);
	destroy_host_buffer(g.rb_sbs);
	destroy_host_buffer(g.rb_presented);
	if (g.target) {
		vkDestroyImage(g.device, g.target, nullptr);
		vkFreeMemory(g.device, g.target_mem, nullptr);
		g.target = VK_NULL_HANDLE;
		g.target_mem = VK_NULL_HANDLE;
	}
}

static bool
create_content(uint32_t w, uint32_t h)
{
	destroy_content();
	g.w = w;
	g.h = h;
	g.generation++;
	if (!create_device_image(w, h, VK_FORMAT_B8G8R8A8_UNORM,
	                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                             VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	                         &g.scene, &g.scene_mem)) {
		return false;
	}
	VkImageViewCreateInfo ivci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	ivci.image = g.scene;
	ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ivci.format = VK_FORMAT_B8G8R8A8_UNORM;
	ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VK_OK(vkCreateImageView(g.device, &ivci, nullptr, &g.scene_view));
	VkFramebufferCreateInfo fbci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
	fbci.renderPass = g.render_pass;
	fbci.attachmentCount = 1;
	fbci.pAttachments = &g.scene_view;
	fbci.width = w;
	fbci.height = h;
	fbci.layers = 1;
	VK_OK(vkCreateFramebuffer(g.device, &fbci, nullptr, &g.framebuffer));
	if (!build_labels()) {
		return false;
	}
	for (int k = 0; k < 2; k++) {
		if (!weave_create_dmabuf_image(g.dd, w, h, VK_FORMAT_B8G8R8A8_UNORM, kWeaveFourccARGB8888,
		                               g.opt.force_linear, g.input[k])) {
			LOGE("dma-buf input allocation failed");
			return false;
		}
	}
	if (g.opt.hud) {
		if (!weave_create_dmabuf_image(g.dd, w, h, VK_FORMAT_B8G8R8A8_UNORM, kWeaveFourccARGB8888,
		                               g.opt.force_linear, g.overlay) ||
		    !create_host_buffer((VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, g.overlay_staging)) {
			return false;
		}
		memset(g.overlay_staging.map, 0, (size_t)g.overlay_staging.size);
		g.overlay_full_upload = true;
		g.overlay_dirty = true;
	}
	if (g.headless && !create_device_image(w, h, VK_FORMAT_B8G8R8A8_UNORM,
	                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	                                       &g.target, &g.target_mem)) {
		return false;
	}
	LOGI(
	    "content %ux%u (generation %u): dma-buf inputs fourcc AR24 modifier "
	    "0x%016llx stride %u%s",
	    w, h, g.generation, (unsigned long long)g.input[0].modifier, g.input[0].stride,
	    g.opt.hud ? ", HUD overlay dma-buf" : "");
	return true;
}

/*
 *
 * Presentation (window) — the app's own swapchain.
 *
 */

static bool
create_surface()
{
#ifdef DXR_APP_HAVE_WAYLAND
	if (g.window.backend() == DxrWindowBackend::Wayland) {
		// The helper's binding struct names the wl_display / wl_surface. It
		// is read here, never chained into xrCreateSession: the runtime is
		// not this window's presenter.
		const auto *b = (const XrWaylandSurfaceBindingCreateInfoDXR *)g.window.session_binding_chain(nullptr);
		VkWaylandSurfaceCreateInfoKHR ci = {VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
		ci.display = b->wlDisplay;
		ci.surface = b->wlSurface;
		VK_OK(vkCreateWaylandSurfaceKHR(g.vk_instance, &ci, nullptr, &g.surface));
		return true;
	}
#endif
	VkXlibSurfaceCreateInfoKHR ci = {VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
	ci.dpy = g.window.x11_display();
	ci.window = g.window.x11_bound_window();
	VK_OK(vkCreateXlibSurfaceKHR(g.vk_instance, &ci, nullptr, &g.surface));
	return true;
}

static void
destroy_swapchain()
{
	if (g.device == VK_NULL_HANDLE) {
		return;
	}
	vkDeviceWaitIdle(g.device);
	for (VkSemaphore s : g.sc_done) {
		vkDestroySemaphore(g.device, s, nullptr);
	}
	g.sc_done.clear();
	g.sc_images.clear();
	if (g.swapchain != VK_NULL_HANDLE) {
		vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
		g.swapchain = VK_NULL_HANDLE;
	}
}

//! (Re)create the swapchain; the content size follows its extent.
static bool
create_swapchain()
{
	VkBool32 supported = VK_FALSE;
	vkGetPhysicalDeviceSurfaceSupportKHR(g.phys, g.qfi, g.surface, &supported);
	if (!supported) {
		LOGE("the graphics queue cannot present to this surface");
		return false;
	}
	VkSurfaceCapabilitiesKHR caps;
	VK_OK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps));
	uint32_t want_w = 0, want_h = 0;
	if (!g.window.current_size(&want_w, &want_h)) {
		want_w = g.opt.width;
		want_h = g.opt.height;
	}
	VkExtent2D ext = caps.currentExtent;
	if (ext.width == UINT32_MAX) {
		// Wayland: the buffer DEFINES the surface; the helper maps a buffer
		// of this DEVICE size onto the logical configure size (wp_viewport).
		ext = {want_w, want_h};
	} else if (ext.width != want_w || ext.height != want_h) {
		LOGI(
		    "surface extent %ux%u (window helper says %ux%u) — following the "
		    "surface",
		    ext.width, ext.height, want_w, want_h);
	}
	if (ext.width == 0 || ext.height == 0) {
		return false; // minimised
	}
	uint32_t fn = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &fn, nullptr);
	std::vector<VkSurfaceFormatKHR> fmts(fn);
	vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &fn, fmts.data());
	// The woven pixels are display-referred bytes, copied verbatim: a BGRA
	// UNORM swapchain matches the output's layout bit for bit.
	VkSurfaceFormatKHR chosen = fmts.empty() ? VkSurfaceFormatKHR{} : fmts[0];
	for (const auto &f : fmts) {
		if (f.format == VK_FORMAT_B8G8R8A8_UNORM) {
			chosen = f;
			break;
		}
		if (f.format == VK_FORMAT_B8G8R8A8_SRGB) {
			chosen = f;
		}
	}
	if (chosen.format != VK_FORMAT_B8G8R8A8_UNORM && chosen.format != VK_FORMAT_B8G8R8A8_SRGB) {
		LOGW(
		    "surface offers no BGRA8 format (first: %d) — the copy will "
		    "reinterpret channels",
		    chosen.format);
	}
	if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
		LOGE("the swapchain cannot be a transfer destination on this surface");
		return false;
	}
	VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if (!(caps.supportedCompositeAlpha & alpha)) {
		alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	}
	uint32_t count = std::max(caps.minImageCount, 3u);
	if (caps.maxImageCount != 0) {
		count = std::min(count, caps.maxImageCount);
	}
	VkSwapchainKHR old = g.swapchain;
	VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	sci.surface = g.surface;
	sci.minImageCount = count;
	sci.imageFormat = chosen.format;
	sci.imageColorSpace = chosen.colorSpace;
	sci.imageExtent = ext;
	sci.imageArrayLayers = 1;
	sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	sci.compositeAlpha = alpha;
	sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	sci.clipped = VK_TRUE;
	sci.oldSwapchain = old;
	VkSwapchainKHR sc = VK_NULL_HANDLE;
	VK_OK(vkCreateSwapchainKHR(g.device, &sci, nullptr, &sc));
	g.swapchain = VK_NULL_HANDLE; // keep `old` alive for the destroy below
	if (old != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(g.device);
		for (VkSemaphore s : g.sc_done) {
			vkDestroySemaphore(g.device, s, nullptr);
		}
		g.sc_done.clear();
		vkDestroySwapchainKHR(g.device, old, nullptr);
	}
	g.swapchain = sc;
	g.sc_format = chosen.format;
	uint32_t n = 0;
	vkGetSwapchainImagesKHR(g.device, sc, &n, nullptr);
	g.sc_images.resize(n);
	vkGetSwapchainImagesKHR(g.device, sc, &n, g.sc_images.data());
	g.sc_done.resize(n);
	VkSemaphoreCreateInfo semci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	for (auto &s : g.sc_done) {
		VK_OK(vkCreateSemaphore(g.device, &semci, nullptr, &s));
	}
	LOGI("swapchain %ux%u, %u images, format %d, FIFO", ext.width, ext.height, n, (int)chosen.format);
	g.swapchain_dirty = false;
	if (ext.width != g.w || ext.height != g.h) {
		return create_content(ext.width, ext.height);
	}
	return true;
}

/*
 *
 * Window geometry -> the weave service (xrWeaveBindWindow2DXR).
 *
 */

//! This frame's client rect, desktop-absolute DEVICE px. False = unknown.
static bool
query_geometry(int32_t *x, int32_t *y, const char **source, WlOwnWindow *wl_out)
{
	if (g.headless) {
		// A fixed, deliberately non-origin spot on the panel, like the probe.
		*x = g.panel_left + 64;
		*y = g.panel_top + 48;
		*source = "headless (fixed)";
		return true;
	}
#ifdef DXR_APP_HAVE_WAYLAND
	if (g.window.backend() == DxrWindowBackend::Wayland) {
		WlOwnWindow ww;
		if (!g.wl_geom_ok || !g.wl_geom.own_window(&ww) || !ww.have_monitor) {
			return false;
		}
		if (wl_out != nullptr) {
			*wl_out = ww;
		}
		const u_wl_monitor mon = {ww.mon_x, ww.mon_y, ww.mon_w, ww.mon_h, ww.mon_scale, 0, 0};
		const u_wl_rect_logical frame = {ww.frame_x, ww.frame_y, ww.frame_w, ww.frame_h};
		const u_wl_rect_logical buffer = {ww.buffer_x, ww.buffer_y, ww.buffer_w, ww.buffer_h};
		u_wl_rect_logical content = frame;
		// The bound surface, not the frame: the CSD bar is a subsurface above it.
		u_wl_window_content_rect(&frame, ww.buffer_w > 0 ? &buffer : nullptr, &content);
		u_wl_rect_px rel = {0, 0, 0, 0};
		if (!u_wl_window_rect_px_on_monitor(&mon, content.logical_x, content.logical_y, content.logical_w,
		                                    content.logical_h, &rel)) {
			return false;
		}
		// Monitor-relative is the exact quantity; the absolute base is the
		// runtime's own panel origin when this IS the panel (so the service's
		// window - panel subtraction is exact), else the monitor's converted
		// origin (a window off the panel has no meaningful phase anyway).
		if (u_wl_monitor_is_panel(&mon, g.panel_left, g.panel_top, g.panel_w, g.panel_h, nullptr)) {
			*x = g.panel_left + rel.x;
			*y = g.panel_top + rel.y;
			*source = "wayland (GetWindows, on the panel)";
		} else {
			u_wl_rect_px m = {0, 0, 0, 0};
			u_wl_monitor_rect_px(&mon, &m);
			*x = m.x + rel.x;
			*y = m.y + rel.y;
			*source = "wayland (GetWindows, NOT on the panel)";
		}
		return true;
	}
#endif
	(void)wl_out;
	Display *dpy = g.window.x11_display();
	::Window child = 0;
	int rx = 0, ry = 0;
	if (dpy == nullptr ||
	    !XTranslateCoordinates(dpy, g.window.x11_bound_window(), DefaultRootWindow(dpy), 0, 0, &rx, &ry, &child)) {
		return false;
	}
	*x = rx;
	*y = ry;
	*source = "x11 (XTranslateCoordinates)";
	return true;
}

//! Re-bind when the rect changed. The runtime dedupes too; this keeps the log
//! to one line per change.
static bool
update_geometry(WlOwnWindow *wl_out, bool *have_wl)
{
	int32_t x = 0, y = 0;
	const char *src = "none";
	*have_wl = false;
	WlOwnWindow ww;
	bool known = query_geometry(&x, &y, &src, &ww);
	if (known && !g.headless && g.window.backend() == DxrWindowBackend::Wayland) {
		*wl_out = ww;
		*have_wl = true;
	}
	if (!known) {
		// Unknown position (Wayland without the GNOME extension): bind at
		// the panel origin — display-scoped, phase-correct only fullscreen
		// on the panel, exactly an Android bind without geometry.
		if (!g.warned_no_geom) {
			g.warned_no_geom = true;
			LOGW(
			    "window position unknown — binding at the panel origin "
			    "(display-scoped weave). On Wayland "
			    "this needs the window-geometry@displayxr.org GNOME Shell "
			    "extension.");
		}
		x = g.panel_left;
		y = g.panel_top;
		src = "unknown (panel origin)";
	}
	if (g.have_geom && x == g.geom_x && y == g.geom_y && g.w == g.geom_w && g.h == g.geom_h) {
		return true;
	}
	XrWeaveWindowGeometryDXR geom = {XR_TYPE_WEAVE_WINDOW_GEOMETRY_DXR};
	geom.windowOriginOnScreen = {x, y};
	geom.clientSize = {(int32_t)g.w, (int32_t)g.h};
	geom.displayId = -1;
	XrWeaveBindWindowInfoDXR bind = {XR_TYPE_WEAVE_BIND_WINDOW_INFO_DXR};
	bind.next = &geom;
	// X11: the XID, recorded by the service for diagnostics only. Wayland: a
	// client cannot name its window to another process -> NULL.
	bind.windowHandle = (!g.headless && g.window.backend() == DxrWindowBackend::X11)
	                        ? (void *)(uintptr_t)g.window.x11_bound_window()
	                        : nullptr;
	XrResult r = g.pfn_bind2(g.session, &bind);
	if (XR_FAILED(r)) {
		LOGE("xrWeaveBindWindow2DXR -> %d", (int)r);
		return false;
	}
	LOGI(
	    "geometry: client (%d, %d) %ux%u desktop px = panel-relative (%d, %d) "
	    "[%s]",
	    x, y, g.w, g.h, x - g.panel_left, y - g.panel_top, src);
	g.have_geom = true;
	g.geom_x = x;
	g.geom_y = y;
	g.geom_w = g.w;
	g.geom_h = g.h;
	g.geom_source = src;
	g.overlay_dirty = true;
	g.weave_snap.set_extent(g.w, g.h);
	return true;
}

/*
 *
 * Wayland drop snap. The compositor ran the drag (on the drag lattice when the
 * extension has one); once the grab ends, ask the display processor for a
 * phase-correct REACHABLE position near the drop and have the compositor move
 * the window there. Same policy as the runtime's in-process
 * vk_wayland_phase_snap (comp_vk_native_compositor.c), through the public
 * entry point instead of the DP vtable.
 *
 */

static bool
dp_snap(int32_t ox, int32_t oy, int32_t tx, int32_t ty, int32_t *sx, int32_t *sy)
{
	XrRect2Di origin = {{ox, oy}, {(int32_t)g.w, (int32_t)g.h}};
	XrRect2Di target = {{tx, ty}, {(int32_t)g.w, (int32_t)g.h}};
	XrRect2Di snapped = {};
	if (XR_FAILED(g.pfn_snap(g.session, &origin, &target, &snapped))) {
		return false;
	}
	*sx = snapped.offset.x;
	*sy = snapped.offset.y;
	return true;
}

static bool
on_lattice(int32_t anchor, int32_t v, uint32_t q)
{
	return q <= 1 || ((v - anchor) % (int32_t)q) == 0;
}

//! vk_snap_search_lattice, over xrWeaveSnapWindowRectDXR. @p exact: a DP fixed
//! point.
static bool
search_reachable(
    int32_t ox, int32_t oy, int32_t cx, int32_t cy, uint32_t q, int32_t *out_x, int32_t *out_y, bool *exact)
{
	*exact = false;
	int32_t sx = cx, sy = cy;
	if (!dp_snap(ox, oy, cx, cy, &sx, &sy)) {
		return false;
	}
	if (on_lattice(cx, sx, q) && on_lattice(cy, sy, q)) {
		*out_x = sx;
		*out_y = sy;
		*exact = true;
		return true;
	}
	const int32_t bx = u_x11_reachable_round(cx, sx, q);
	const int32_t by = u_x11_reachable_round(cy, sy, q);
	const uint32_t n = u_x11_lattice_candidate_count(3);
	for (uint32_t k = 0; k < n; k++) {
		int32_t i = 0, j = 0;
		u_x11_lattice_candidate(k, &i, &j);
		const int32_t px = bx + i * (int32_t)q, py = by + j * (int32_t)q;
		int32_t rx = px, ry = py;
		if (dp_snap(ox, oy, px, py, &rx, &ry) && on_lattice(cx, rx, q) && on_lattice(cy, ry, q)) {
			*out_x = rx;
			*out_y = ry;
			*exact = true;
			return true;
		}
	}
	*out_x = bx;
	*out_y = by;
	return true;
}

static void
wl_drop_snap(const WlOwnWindow &ww)
{
	auto &s = g.ds;
	const int32_t cx = g.geom_x, cy = g.geom_y;
	if (s.verify) {
		if (ww.have_moving && ww.moving) {
			s.verify = false; // grabbed again before our move landed: a new drag
		} else {
			const bool arrived = cx == s.want_x && cy == s.want_y;
			const bool changed = cx != s.last_x || cy != s.last_y;
			s.last_x = cx;
			s.last_y = cy;
			if (!arrived) {
				if (changed) {
					s.verify_polls = 0;
				}
				if (++s.verify_polls < 30) {
					return;
				}
			}
			s.verify = false;
			if (arrived) {
				LOGI("drop snap: LANDED at (%d, %d) px, where the lattice asked", cx, cy);
			} else {
				LOGW("drop snap: asked for (%d, %d) px, the window LANDED at (%d, %d)", s.want_x,
				     s.want_y, cx, cy);
			}
			s.anchor_x = s.last_x = cx;
			s.anchor_y = s.last_y = cy;
			s.moving = false;
			s.still = 0;
			return;
		}
	}
	if (!s.have_last) {
		s.have_last = true;
		s.last_x = s.anchor_x = cx;
		s.last_y = s.anchor_y = cy;
		return;
	}
	if (cx != s.last_x || cy != s.last_y) {
		if (!s.moving) {
			s.anchor_x = s.last_x; // the drag's origin is the phase reference
			s.anchor_y = s.last_y;
			s.moving = true;
			s.tries = 0;
		}
		s.still = 0;
		s.last_x = cx;
		s.last_y = cy;
		return;
	}
	if (!s.moving) {
		return;
	}
	if (ww.have_moving && ww.moving) {
		s.still = 0; // held, not dropped
		return;
	}
	if (!ww.have_moving && ++s.still < 6) {
		return; // old publisher: settle on stillness
	}
	s.moving = false;
	s.still = 0;
	if (ww.lattice_drop) {
		LOGI(
		    "drop snap: dropped at (%d, %d) px on the drag lattice — accepted as "
		    "is",
		    cx, cy);
		return;
	}
	uint32_t q = 0;
	if (!u_wl_placement_quantum(ww.mon_scale, 0.01, &q)) {
		if (!s.warned_scale) {
			s.warned_scale = true;
			LOGW(
			    "drop snap: monitor scale %.4f is not an integer — no reachable "
			    "lattice; the window keeps "
			    "the phase it was dropped on",
			    ww.mon_scale);
		}
		return;
	}
	int32_t sx = cx, sy = cy;
	bool exact = false;
	if (!search_reachable(s.anchor_x, s.anchor_y, cx, cy, q, &sx, &sy, &exact)) {
		return; // the DP does not snap
	}
	if (!exact) {
		LOGW(
		    "drop snap: NOT MOVING — no reachable position near (%d, %d) is "
		    "phase-correct (quantum %u)",
		    cx, cy, q);
		return;
	}
	if (sx == cx && sy == cy) {
		LOGI("drop snap: dropped at (%d, %d) px, already on the lattice", cx, cy);
		return;
	}
	if (s.tries >= 2) {
		return;
	}
	s.tries++;
	const int32_t mdx = (sx - cx) / (int32_t)q, mdy = (sy - cy) / (int32_t)q;
	const bool ok = g.wl_geom.move_window(ww.frame_x + mdx, ww.frame_y + mdy);
	LOGI(
	    "drop snap: drop at (%d, %d) px -> phase-correct (%d, %d); MoveWindow "
	    "by (%d, %d) logical px%s",
	    cx, cy, sx, sy, mdx, mdy, ok ? "" : " — REFUSED");
	if (ok) {
		s.verify = true;
		s.verify_polls = 0;
		s.want_x = sx;
		s.want_y = sy;
	}
}

/*
 *
 * HUD (the 2D overlay the display processor composites OVER the weave).
 *
 */

static void
draw_hud()
{
	if (!g.opt.hud || g.overlay_staging.map == nullptr) {
		return;
	}
	const int s = font_scale(g.h);
	char lines[7][96];
	snprintf(lines[0], sizeof(lines[0]), "WEAVE PRESENT VK LINUX");
	snprintf(lines[1], sizeof(lines[1]), "SHOWING: %s", g.opt.sbs ? "SBS INPUT (UNWOVEN)" : "WOVEN OUTPUT");
	snprintf(lines[2], sizeof(lines[2]), "FRAME %llu  %.0f FPS", (unsigned long long)g.frame, g.fps);
	snprintf(lines[3], sizeof(lines[3]), "EYE L %+.3f %+.3f %+.3f %s", (double)g.eyes[0].x, (double)g.eyes[0].y,
	         (double)g.eyes[0].z, !g.eyes_valid ? "NOMINAL" : (g.eyes_tracking ? "TRACKED" : "FALLBACK"));
	snprintf(lines[4], sizeof(lines[4]), "PANEL ORIGIN %d,%d  %uX%u", g.geom_x - g.panel_left,
	         g.geom_y - g.panel_top, g.w, g.h);
	snprintf(lines[5], sizeof(lines[5]), "OUT MOD %016llX", (unsigned long long)g.out_modifier);
	snprintf(lines[6], sizeof(lines[6]), "ESC QUIT  F11 FULLSCREEN  S SBS");
	int tw = 0;
	for (auto &l : lines) {
		tw = std::max(tw, text5x7::width(l, s));
	}
	const int pad = 4 * s;
	const int lh = (text5x7::kHeight + 3) * s;
	const int bw = std::min((int)g.w - 2 * pad, tw + 2 * pad);
	const int bh = std::min((int)g.h / 2, 7 * lh + 2 * pad);
	// Top-right, clear of the eye labels on the left.
	const int bx = std::max(0, (int)g.w - bw - 6 * s);
	const int by = 6 * s;
	// Clear the previous box, then draw the new one. Opaque on purpose: a 2D
	// HUD over the weave must read crisp and must hide the interlace under it.
	if (g.hud_w > 0) {
		text5x7::fill(g.overlay_staging.map, (int)g.w, (int)g.h, g.hud_x, g.hud_y, g.hud_w, g.hud_h, 0, 0, 0,
		              0);
	}
	text5x7::fill(g.overlay_staging.map, (int)g.w, (int)g.h, bx, by, bw, bh, 20, 22, 30, 255);
	for (int i = 0; i < 7; i++) {
		const uint8_t r = i == 0 ? 255 : 220, gg = i == 0 ? 200 : 230, b = i == 0 ? 60 : 240;
		text5x7::draw(g.overlay_staging.map, (int)g.w, (int)g.h, bx + pad, by + pad + i * lh, lines[i], s, s, r,
		              gg, b, 255);
	}
	// The whole overlay is re-uploaded (render_and_release): ~2 Hz, cheap.
	g.hud_x = bx;
	g.hud_y = by;
	g.hud_w = bw;
	g.hud_h = bh;
}

/*
 *
 * The frame.
 *
 */

static void
fill_push(ScenePush &p, double t)
{
	const float px = g.panel_w_m / (float)g.panel_w; // metres per device px
	const float py = g.panel_h_m / (float)g.panel_h;
	const float ox = (float)(g.geom_x - g.panel_left), oy = (float)(g.geom_y - g.panel_top);
	// Canvas = the window's client rect ON the display plane (display space:
	// origin at the panel centre, y up, metres).
	const float cx0 = -0.5f * g.panel_w_m + ox * px;
	const float cy0 = 0.5f * g.panel_h_m - oy * py;
	const float cw = (float)g.w * px, ch = (float)g.h * py;
	const XrVector3f &l = g.eyes[0], &r = g.eyes[1];
	p = {{l.x, l.y, l.z, 0.0f},
	     {r.x, r.y, r.z, 0.0f},
	     {cx0, cy0, cw, ch},
	     {cx0 + 0.5f * cw, cy0 - 0.44f * ch, 0.0f, 0.2f * ch},
	     {(float)(t * 0.8), (float)g.w, (float)g.h, 0.12f}};
}

//! Record + submit the render: scene -> labels -> input[k] (+ HUD -> overlay),
//! released to FOREIGN.
static bool
render_and_release(int k, bool upload_overlay, int *acq_fd)
{
	VkCommandBuffer cmd = g.render_cmd;
	if (!begin_cmd(cmd)) {
		return false;
	}
	ScenePush push;
	fill_push(push, (double)(now_ns() - g.fps_t0) * 1e-9);
	VkRenderPassBeginInfo rpbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
	rpbi.renderPass = g.render_pass;
	rpbi.framebuffer = g.framebuffer;
	rpbi.renderArea = {{0, 0}, {g.w, g.h}};
	vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline);
	VkViewport vp = {0.0f, 0.0f, (float)g.w, (float)g.h, 0.0f, 1.0f};
	VkRect2D sc = {{0, 0}, {g.w, g.h}};
	vkCmdSetViewport(cmd, 0, 1, &vp);
	vkCmdSetScissor(cmd, 0, 1, &sc);
	vkCmdPushConstants(cmd, g.pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd); // -> TRANSFER_DST_OPTIMAL

	if (!g.label_regions.empty()) {
		std::vector<VkBufferImageCopy> regions;
		for (const auto &lr : g.label_regions) {
			VkBufferImageCopy c = {};
			c.bufferOffset = lr.offset;
			c.bufferRowLength = (uint32_t)lr.w;
			c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			c.imageOffset = {lr.x, lr.y, 0};
			c.imageExtent = {(uint32_t)lr.w, (uint32_t)lr.h, 1};
			regions.push_back(c);
		}
		vkCmdCopyBufferToImage(cmd, g.labels.buffer, g.scene, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                       (uint32_t)regions.size(), regions.data());
	}
	image_barrier(cmd, g.scene, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	              VK_PIPELINE_STAGE_TRANSFER_BIT);

	DmabufImage &in = g.input[k];
	if (in.owned_by_us) {
		image_barrier(cmd, in.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		              VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		              VK_PIPELINE_STAGE_TRANSFER_BIT);
	} else {
		weave_foreign_barrier(cmd, g.qfi, in.image, true, VK_IMAGE_LAYOUT_GENERAL,
		                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		                      VK_PIPELINE_STAGE_TRANSFER_BIT);
	}
	VkImageCopy ic = {};
	ic.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	ic.extent = {g.w, g.h, 1};
	vkCmdCopyImage(cmd, g.scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, in.image,
	               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
	weave_foreign_barrier(cmd, g.qfi, in.image, false, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                      VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	in.owned_by_us = false;

	if (upload_overlay && g.overlay.image != VK_NULL_HANDLE) {
		DmabufImage &ov = g.overlay;
		if (ov.owned_by_us) {
			image_barrier(cmd, ov.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
			              VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			              VK_PIPELINE_STAGE_TRANSFER_BIT);
		} else {
			weave_foreign_barrier(cmd, g.qfi, ov.image, true, VK_IMAGE_LAYOUT_GENERAL,
			                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
			                      VK_PIPELINE_STAGE_TRANSFER_BIT);
		}
		VkBufferImageCopy c = {};
		c.bufferRowLength = g.w;
		c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		c.imageExtent = {g.w, g.h, 1};
		vkCmdCopyBufferToImage(cmd, g.overlay_staging.buffer, ov.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
		                       &c);
		weave_foreign_barrier(cmd, g.qfi, ov.image, false, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                      VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		                      VK_PIPELINE_STAGE_TRANSFER_BIT);
		ov.owned_by_us = false;
	}
	VK_OK(vkEndCommandBuffer(cmd));
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores = &g.acq_sem;
	VK_OK(vkQueueSubmit(g.queue, 1, &si, g.render_fence));
	g.render_pending = true;
	VkSemaphoreGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
	gi.semaphore = g.acq_sem;
	gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
	VK_OK(g.dd.get_sem_fd(g.device, &gi, acq_fd));
	return true;
}

/*!
 * xrWeaveSubmitDXR, spec v10. Every fd passed in is the runtime's on success
 * and still ours otherwise. @p release_fd receives this frame's release fence.
 */
static XrResult
weave_submit(int k, bool with_overlay, int acq_fd, int *release_fd)
{
	*release_fd = -1;
	XrRect2Di whole = {{0, 0}, {(int32_t)g.w, (int32_t)g.h}};
	XrWeaveSubmitSyncDXR sync = {XR_TYPE_WEAVE_SUBMIT_SYNC_DXR};
	sync.acquireFenceFd = acq_fd;
	XrWeaveDmabufDescDXR in_desc = {XR_TYPE_WEAVE_DMABUF_DESC_DXR};
	// bufferId: stable per pool slot AND per allocation generation, so a
	// reallocated buffer never reuses a cached import.
	weave_fill_dmabuf_desc(in_desc, g.input[k], dup(g.input[k].fd),
	                       ((uint64_t)g.generation << 8) | (uint64_t)(k + 1));
	XrWeaveOverlayDmabufDescDXR ov_desc = {XR_TYPE_WEAVE_OVERLAY_DMABUF_DESC_DXR};
	XrWeaveSubmitOverlaysDXR ov = {XR_TYPE_WEAVE_SUBMIT_OVERLAYS_DXR};
	ov.rectCount = 0; // whole-window premultiplied atlas: alpha is the 2D mask
	XrWeaveSubmitRectsDXR batch = {XR_TYPE_WEAVE_SUBMIT_RECTS_DXR};
	batch.rectCount = 1;
	batch.rects = &whole;
	// chain: submit -> batch -> [ov -> ov_desc] -> in_desc -> sync
	in_desc.next = &sync;
	if (with_overlay) {
		weave_fill_overlay_desc(ov_desc, g.overlay, dup(g.overlay.fd), ((uint64_t)g.generation << 8) | 0x80);
		ov_desc.next = &in_desc;
		ov.next = &ov_desc;
		batch.next = &ov;
	} else {
		batch.next = &in_desc;
	}
	XrWeaveSubmitInfoDXR submit = {XR_TYPE_WEAVE_SUBMIT_INFO_DXR};
	submit.next = &batch;
	submit.firstChunk = XR_TRUE;

	XrWeaveOutputSyncDXR out_sync = {XR_TYPE_WEAVE_OUTPUT_SYNC_DXR};
	out_sync.releaseFenceFd = -1;
	XrWeaveOutputDmabufDXR out_dmabuf = {XR_TYPE_WEAVE_OUTPUT_DMABUF_DXR};
	out_dmabuf.fd = -1;
	out_dmabuf.next = &out_sync;
	XrWeaveOutputDXR out = {XR_TYPE_WEAVE_OUTPUT_DXR};
	out.next = &out_dmabuf;
	XrResult r = g.pfn_submit(g.session, &submit, &out);
	if (XR_FAILED(r)) {
		close(in_desc.fd);
		if (with_overlay) {
			close(ov_desc.fd);
		}
		close(acq_fd);
		return r;
	}
	if (out.eyeCount >= 2 && out.eyesValid) {
		g.eyes[0] = out.eyes[0];
		g.eyes[1] = out.eyes[1];
		g.eyes_valid = true;
		g.eyes_tracking = out.eyesTracking == XR_TRUE;
	}
	if (out_dmabuf.fd >= 0) {
		// First frame, or the service reallocated (resize): re-import.
		vkDeviceWaitIdle(g.device);
		weave_destroy_dmabuf_image(g.device, g.output);
		if (!weave_import_dmabuf_output(g.dd, out_dmabuf, g.output)) {
			close(out_dmabuf.fd);
			if (out_sync.releaseFenceFd >= 0) {
				close(out_sync.releaseFenceFd);
			}
			LOGE("could not import the woven dma-buf");
			return XR_ERROR_RUNTIME_FAILURE;
		}
		g.output.owned_by_us = false; // the service's; acquired from FOREIGN per use
		g.out_modifier = out_dmabuf.drmModifier;
		g.overlay_dirty = true;
		LOGI(
		    "woven output dma-buf %ux%u fourcc %.4s modifier 0x%016llx stride %u "
		    "size %llu (frame %llu)",
		    out_dmabuf.width, out_dmabuf.height, (const char *)&out_dmabuf.drmFourcc,
		    (unsigned long long)out_dmabuf.drmModifier, out_dmabuf.strides[0],
		    (unsigned long long)out_dmabuf.size, (unsigned long long)g.frame);
	}
	*release_fd = out_sync.releaseFenceFd;
	return XR_SUCCESS;
}

//! Copy @p src (w x h, in @p src_layout) into @p dst (TRANSFER_DST_OPTIMAL),
//! clearing any margin.
static void
copy_into(VkCommandBuffer cmd,
          VkImage src,
          VkImageLayout src_layout,
          uint32_t sw,
          uint32_t sh,
          VkImage dst,
          uint32_t dw,
          uint32_t dh)
{
	if (sw < dw || sh < dh) {
		VkClearColorValue black = {{0.0f, 0.0f, 0.0f, 1.0f}};
		VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
		VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
		                     nullptr, 0, nullptr);
	}
	VkImageCopy ic = {};
	ic.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	ic.extent = {std::min(sw, dw), std::min(sh, dh), 1};
	vkCmdCopyImage(cmd, src, src_layout, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
}

static void
copy_to_buffer(VkCommandBuffer cmd, VkImage src, VkImageLayout layout, uint32_t w, uint32_t h, HostBuffer &dst)
{
	VkBufferImageCopy c = {};
	c.bufferRowLength = g.w; // the readback is content-sized even when the source lags a resize
	c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	c.imageExtent = {w, h, 1};
	vkCmdCopyImageToBuffer(cmd, src, layout, dst.buffer, 1, &c);
}

/*!
 * The present submit: GPU-wait the release fence, take the woven output from
 * FOREIGN, copy it (or, with --sbs, the unwoven scene) into the presentable
 * image, hand the output back. @p dump also reads back woven + SBS.
 */
static bool
present(int release_fd, bool dump)
{
	bool wait_rel = false;
	if (release_fd >= 0) {
		VkImportSemaphoreFdInfoKHR ii = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
		ii.semaphore = g.rel_sem;
		ii.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
		ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		ii.fd = release_fd;
		if (g.dd.import_sem_fd(g.device, &ii) != VK_SUCCESS) {
			close(release_fd);
			LOGE("release sync_file import failed");
			return false;
		}
		wait_rel = true; // Vulkan owns release_fd now
	}

	uint32_t img_index = 0;
	VkImage dst = g.target;
	uint32_t dw = g.w, dh = g.h;
	bool acquired = false;
	if (!g.headless) {
		VkResult ar = vkAcquireNextImageKHR(g.device, g.swapchain, 1000000000ULL, g.img_avail, VK_NULL_HANDLE,
		                                    &img_index);
		if (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR) {
			acquired = true;
			dst = g.sc_images[img_index];
			if (ar == VK_SUBOPTIMAL_KHR) {
				g.swapchain_dirty = true;
			}
		} else {
			g.swapchain_dirty = true;
		}
	}

	VkCommandBuffer cmd = g.present_cmd;
	if (!begin_cmd(cmd)) {
		return false;
	}
	const bool have_dst = g.headless || acquired;
	const bool have_out = g.output.image != VK_NULL_HANDLE;
	if (have_out) {
		weave_foreign_barrier(cmd, g.qfi, g.output.image, true, VK_IMAGE_LAYOUT_GENERAL,
		                      VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
		                      VK_PIPELINE_STAGE_TRANSFER_BIT);
	}
	if (have_dst) {
		image_barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		              VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		              VK_PIPELINE_STAGE_TRANSFER_BIT);
		if (g.opt.sbs || !have_out) {
			copy_into(cmd, g.scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.w, g.h, dst, dw, dh);
		} else {
			copy_into(cmd, g.output.image, VK_IMAGE_LAYOUT_GENERAL, g.output.w, g.output.h, dst, dw, dh);
		}
		image_barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		              g.headless ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		              VK_ACCESS_TRANSFER_WRITE_BIT, g.headless ? VK_ACCESS_TRANSFER_READ_BIT : 0,
		              VK_PIPELINE_STAGE_TRANSFER_BIT,
		              g.headless ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	}
	if (dump && have_out) {
		const VkDeviceSize sz = (VkDeviceSize)g.w * g.h * 4;
		auto ensure = [&](HostBuffer &b) {
			if (b.size == sz) {
				return true;
			}
			destroy_host_buffer(b);
			return create_host_buffer(sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT, b);
		};
		if (ensure(g.rb_woven) && ensure(g.rb_sbs) && (!g.headless || ensure(g.rb_presented))) {
			copy_to_buffer(cmd, g.output.image, VK_IMAGE_LAYOUT_GENERAL, std::min(g.output.w, g.w),
			               std::min(g.output.h, g.h), g.rb_woven);
			copy_to_buffer(cmd, g.scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.w, g.h, g.rb_sbs);
			if (g.headless) {
				// What was "presented": proves the present copy, not just the weave.
				copy_to_buffer(cmd, g.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.w, g.h,
				               g.rb_presented);
			}
		}
	}
	if (have_out) {
		weave_foreign_barrier(cmd, g.qfi, g.output.image, false, VK_IMAGE_LAYOUT_GENERAL,
		                      VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
		                      VK_PIPELINE_STAGE_TRANSFER_BIT);
	}
	VK_OK(vkEndCommandBuffer(cmd));

	VkSemaphore waits[2];
	VkPipelineStageFlags stages[2];
	uint32_t nw = 0;
	if (wait_rel) {
		waits[nw] = g.rel_sem;
		stages[nw++] = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	if (acquired) {
		waits[nw] = g.img_avail;
		stages[nw++] = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.waitSemaphoreCount = nw;
	si.pWaitSemaphores = waits;
	si.pWaitDstStageMask = stages;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	if (acquired) {
		si.signalSemaphoreCount = 1;
		si.pSignalSemaphores = &g.sc_done[img_index];
	}
	VK_OK(vkQueueSubmit(g.queue, 1, &si, g.present_fence));
	g.present_pending = true;
	if (acquired) {
		VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &g.sc_done[img_index];
		pi.swapchainCount = 1;
		pi.pSwapchains = &g.swapchain;
		pi.pImageIndices = &img_index;
		VkResult pr = vkQueuePresentKHR(g.queue, &pi);
		if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
			g.swapchain_dirty = true;
		} else if (pr != VK_SUCCESS) {
			LOGE("vkQueuePresentKHR -> %d", (int)pr);
			return false;
		}
	}
	return true;
}

/*
 *
 * Verification: the woven frame against the SBS input it was made from.
 *
 */

struct Verdict
{
	double woven_luma = 0.0;     //!< mean luma of the woven frame
	double nonblack = 0.0;       //!< fraction of woven pixels with any channel > 8
	double diff_vs_sbs = 0.0;    //!< mean |woven - sbs| per channel: did the weave
	                             //!< transform anything?
	double anaglyph_err = 0.0;   //!< mean |woven - anaglyph(sbs)| per channel, outside the HUD
	bool hud_ok = true;          //!< a HUD-box pixel reads the HUD colour (2D composited
	                             //!< over the weave)
	double presented_err = -1.0; //!< (headless) mean |presented - (sbs mode ? sbs : woven)|
};

static void
px(const uint8_t *buf, uint32_t w, int x, int y, int *r, int *gg, int *b)
{
	const uint8_t *p = buf + ((size_t)y * w + (size_t)x) * 4; // BGRA
	*b = p[0];
	*gg = p[1];
	*r = p[2];
}

static Verdict
analyze(const uint8_t *woven, const uint8_t *sbs, uint32_t w, uint32_t h)
{
	Verdict v;
	const uint32_t ew = w / 2;
	double luma = 0.0, diff = 0.0, aerr = 0.0;
	uint64_t n = 0, nb = 0, na = 0;
	const bool hud = g.opt.hud && g.hud_w > 0;
	for (uint32_t y = 1; y + 1 < h; y += 2) {
		for (uint32_t x = 1; x + 1 < w; x += 2) {
			int wr, wg, wb, sr, sg, sb;
			px(woven, w, (int)x, (int)y, &wr, &wg, &wb);
			px(sbs, w, (int)x, (int)y, &sr, &sg, &sb);
			luma += 0.2126 * wr + 0.7152 * wg + 0.0722 * wb;
			nb += (wr > 8 || wg > 8 || wb > 8) ? 1 : 0;
			diff += (std::abs(wr - sr) + std::abs(wg - sg) + std::abs(wb - sb)) / 3.0;
			n++;
			const bool in_hud = hud && (int)x >= g.hud_x - 1 && (int)x < g.hud_x + g.hud_w + 1 &&
			                    (int)y >= g.hud_y - 1 && (int)y < g.hud_y + g.hud_h + 1;
			if (in_hud) {
				continue;
			}
			// sim_display anaglyph: red from the left tile, green+blue from
			// the right, each sampled (bilinear) at u/2 — take the best of the
			// two nearest source columns.
			double best = 1e9;
			for (int dx = -1; dx <= 1; dx++) {
				const int lx = std::clamp((int)(x / 2) + dx, 0, (int)ew - 1);
				int lr, lg, lb, rr, rg, rb;
				px(sbs, w, lx, (int)y, &lr, &lg, &lb);
				px(sbs, w, (int)ew + lx, (int)y, &rr, &rg, &rb);
				const double e = (std::abs(wr - lr) + std::abs(wg - rg) + std::abs(wb - rb)) / 3.0;
				best = std::min(best, e);
			}
			aerr += best;
			na++;
		}
	}
	v.woven_luma = n ? luma / (double)n : 0.0;
	v.nonblack = n ? (double)nb / (double)n : 0.0;
	v.diff_vs_sbs = n ? diff / (double)n : 0.0;
	v.anaglyph_err = na ? aerr / (double)na : 0.0;
	if (hud) {
		// A pixel inside the HUD box, below the text lines' left margin.
		int r, gg, b;
		px(woven, w, g.hud_x + 1, g.hud_y + g.hud_h - 2, &r, &gg, &b);
		v.hud_ok = std::abs(r - 20) <= 3 && std::abs(gg - 22) <= 3 && std::abs(b - 30) <= 3;
	}
	return v;
}

static bool
write_png(const std::string &path, const uint8_t *bgra, uint32_t w, uint32_t h)
{
	std::vector<uint8_t> rgb((size_t)w * h * 3);
	for (size_t i = 0; i < (size_t)w * h; i++) {
		rgb[i * 3 + 0] = bgra[i * 4 + 2];
		rgb[i * 3 + 1] = bgra[i * 4 + 1];
		rgb[i * 3 + 2] = bgra[i * 4 + 0];
	}
	const bool ok = stbi_write_png(path.c_str(), (int)w, (int)h, 3, rgb.data(), (int)w * 3) != 0;
	LOGI("%s %s (%ux%u)", ok ? "wrote" : "FAILED to write", path.c_str(), w, h);
	return ok;
}

//! Write the pair of PNGs + log the verdict. Called after the dump frame's
//! fence.
static Verdict
dump_and_analyze(const char *tag)
{
	const std::string base = g.opt.dump_dir + "/weave_present_";
	write_png(base + "woven.png", g.rb_woven.map, g.w, g.h);
	write_png(base + "sbs.png", g.rb_sbs.map, g.w, g.h);
	Verdict v = analyze(g.rb_woven.map, g.rb_sbs.map, g.w, g.h);
	if (g.headless && g.rb_presented.map != nullptr) {
		write_png(base + "presented.png", g.rb_presented.map, g.w, g.h);
		const uint8_t *want = g.opt.sbs ? g.rb_sbs.map : g.rb_woven.map;
		double e = 0.0;
		const size_t n = (size_t)g.w * g.h * 4;
		for (size_t i = 0; i < n; i++) {
			if ((i & 3) != 3) {
				e += std::abs((int)g.rb_presented.map[i] - (int)want[i]);
			}
		}
		v.presented_err = e / (double)((size_t)g.w * g.h * 3);
		LOGI("presented frame vs the %s: mean |diff| %.3f", g.opt.sbs ? "SBS input (--sbs)" : "woven output",
		     v.presented_err);
	}
	LOGI(
	    "%s: woven mean luma %.1f, non-black %.1f%%, mean |woven - sbs| %.1f, "
	    "mean |woven - anaglyph(sbs)| %.1f, "
	    "HUD %s",
	    tag, v.woven_luma, v.nonblack * 100.0, v.diff_vs_sbs, v.anaglyph_err,
	    !g.opt.hud ? "off" : (v.hud_ok ? "composited" : "MISSING"));
	return v;
}

/*
 *
 * Main loop.
 *
 */

static bool
poll_xr_events()
{
	XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
	while (xrPollEvent(g.instance, &ev) == XR_SUCCESS) {
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			const auto *s = (const XrEventDataSessionStateChanged *)&ev;
			if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
				LOGW("session state %d — leaving", (int)s->state);
				return false;
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
			LOGW("instance loss pending — leaving");
			return false;
		}
		ev = {XR_TYPE_EVENT_DATA_BUFFER};
	}
	return true;
}

static bool
dump_trigger_present()
{
	const std::string trig = g.opt.dump_dir + "/weave_present_trigger";
	struct stat st;
	if (stat(trig.c_str(), &st) != 0) {
		return false;
	}
	unlink(trig.c_str());
	return true;
}

static int
run()
{
	const int total = g.headless ? g.opt.headless_frames : -1;
	const int dump_frame = g.headless ? std::max(1, total - 2) : -1;
	int fds_early = -1, fds_late = -1, svc_early = -1, svc_late = -1;
	bool running = true;
	g.fps_t0 = now_ns();
	int64_t fps_mark = g.fps_t0;
	bool pass = true;
	Verdict headless_verdict;

	for (int f = 0; running && !g_quit && (total < 0 || f < total); f++) {
		if (!wait_frame_fences()) {
			return 1;
		}
		// A frame's dump readback is complete once its fences are waited.
		if (g.dump_requested) {
			g.dump_requested = false;
			Verdict v = dump_and_analyze(g.headless ? "headless check" : "anaglyph check");
			if (g.headless) {
				headless_verdict = v;
			}
		}
		if (!poll_xr_events()) {
			break;
		}

		if (!g.headless) {
			g.window.pump_events(
			    [&](const DxrWindowEvent &e) {
				    if (e.type == DxrWindowEvent::Type::KeyDown && !e.repeat) {
					    if (e.keysym == XK_Escape) {
						    running = false;
					    } else if (e.keysym == XK_s) {
						    g.opt.sbs = !g.opt.sbs;
						    g.overlay_dirty = true;
						    LOGI("presenting the %s",
						         g.opt.sbs ? "UNWOVEN SBS input" : "woven output");
					    }
				    } else if (e.type == DxrWindowEvent::Type::Resize) {
					    g.swapchain_dirty = true;
				    }
			    },
			    &running);
			if (!running) {
				break;
			}
			if (g.swapchain_dirty) {
				uint32_t cw = 0, ch = 0;
				if (g.window.current_size(&cw, &ch) && (cw == 0 || ch == 0)) {
					continue; // minimised
				}
				if (!create_swapchain()) {
					LOGE("swapchain recreation failed");
					return 1;
				}
			}
		}

		if (g.headless && f == g.opt.resize_frame && g.opt.resize_w >= 64 && g.opt.resize_h >= 64) {
			LOGI("test resize at frame %d: %ux%u -> %ux%u", f, g.w, g.h, g.opt.resize_w, g.opt.resize_h);
			if (!create_content(g.opt.resize_w, g.opt.resize_h)) {
				return 1;
			}
			fds_early = -1; // re-baseline after the reallocation
		}
		if (g.headless && fds_early < 0 && f >= std::max(10, g.opt.resize_frame + 10)) {
			fds_early = weave_count_fds(0);
			svc_early = g.opt.service_pid > 0 ? weave_count_fds(g.opt.service_pid) : -1;
		}

		WlOwnWindow ww;
		bool have_wl = false;
		if (!update_geometry(&ww, &have_wl)) {
			return 1;
		}
		if (have_wl) {
			wl_drop_snap(ww);
		}

		// HUD at ~2 Hz (and on any state change).
		const int64_t t = now_ns();
		if (t - fps_mark >= 1000000000LL) {
			g.fps = (double)(g.frame - g.fps_frames) * 1e9 / (double)(t - fps_mark);
			g.fps_frames = g.frame;
			fps_mark = t;
		}
		bool upload_overlay = false;
		if (g.opt.hud && (g.overlay_dirty || t - g.hud_t >= 500000000LL)) {
			draw_hud();
			g.hud_t = t;
			g.overlay_dirty = false;
			upload_overlay = true;
		}
		if (g.overlay_full_upload) {
			upload_overlay = g.opt.hud;
			g.overlay_full_upload = false;
		}

		const int k = (int)(g.frame & 1);
		int acq_fd = -1;
		if (!render_and_release(k, upload_overlay, &acq_fd)) {
			return 1;
		}
		int release_fd = -1;
		XrResult r = weave_submit(k, g.opt.hud, acq_fd, &release_fd);
		if (XR_FAILED(r)) {
			// Our acquire fence was consumed by nobody: wait the render out so
			// the next frame starts clean.
			LOGE("xrWeaveSubmitDXR -> %d (frame %llu)", (int)r, (unsigned long long)g.frame);
			if (r == XR_ERROR_SESSION_LOST || r == XR_ERROR_INSTANCE_LOST || ++g.submit_failures > 10) {
				return 1;
			}
			g.frame++;
			continue;
		}
		g.submit_failures = 0;
		bool dump = false;
		if (g.headless && f == dump_frame) {
			dump = true;
		} else if (g.opt.anaglyph_check && dump_trigger_present()) {
			dump = true;
		}
		if (!present(release_fd, dump)) {
			return 1;
		}
		g.dump_requested = dump;
		g.frame++;

		if (g.headless && f == 0) {
			LOGI("frame 0: submitted + presented (release fence waited on the GPU)");
		}
	}
	vkDeviceWaitIdle(g.device);
	if (g.dump_requested) {
		g.dump_requested = false;
		Verdict v = dump_and_analyze(g.headless ? "headless check" : "anaglyph check");
		if (g.headless) {
			headless_verdict = v;
		}
	}
	if (!g.headless) {
		return 0;
	}

	// ---- Headless self-check.
	fds_late = weave_count_fds(0);
	svc_late = g.opt.service_pid > 0 ? weave_count_fds(g.opt.service_pid) : -1;
	const Verdict &v = headless_verdict;
	auto check = [&](bool ok, const char *what) {
		LOGI("  %-52s %s", what, ok ? "OK" : "FAIL");
		pass = pass && ok;
	};
	LOGI("headless self-check over %d frames:", total);
	check(g.output.image != VK_NULL_HANDLE, "woven dma-buf received + imported");
	if (g.opt.resize_frame >= 0) {
		check(g.output.w == g.w && g.output.h == g.h, "woven output follows the resize");
	}
	check(v.presented_err >= 0.0 && v.presented_err < 0.01,
	      g.opt.sbs ? "presented frame == SBS input (--sbs)" : "presented frame == woven output");
	check(v.nonblack > 0.5 && v.woven_luma > 10.0, "woven frame is not black");
	check(v.diff_vs_sbs > 5.0, "woven frame differs from the SBS input (weave ran)");
	if (g.opt.hud) {
		check(v.hud_ok, "HUD overlay composited over the weave");
	}
	if (g.opt.expect_anaglyph) {
		check(v.anaglyph_err < 10.0, "woven == red/cyan anaglyph of the SBS input");
	}
	LOGI(
	    "fd counts (steady state) -> frame %d: app %d -> %d, service(pid %ld) "
	    "%d -> %d",
	    total, fds_early, fds_late, g.opt.service_pid, svc_early, svc_late);
	check(fds_early == fds_late && svc_early == svc_late, "fd counts flat");
	LOGI("%s", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}

static void
cleanup()
{
	if (g.device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(g.device);
	}
	destroy_content();
	if (g.device != VK_NULL_HANDLE) {
		weave_destroy_dmabuf_image(g.device, g.output);
		destroy_swapchain();
		if (g.pipeline) {
			vkDestroyPipeline(g.device, g.pipeline, nullptr);
		}
		if (g.pipeline_layout) {
			vkDestroyPipelineLayout(g.device, g.pipeline_layout, nullptr);
		}
		if (g.render_pass) {
			vkDestroyRenderPass(g.device, g.render_pass, nullptr);
		}
		vkDestroySemaphore(g.device, g.acq_sem, nullptr);
		vkDestroySemaphore(g.device, g.rel_sem, nullptr);
		vkDestroySemaphore(g.device, g.img_avail, nullptr);
		vkDestroyFence(g.device, g.render_fence, nullptr);
		vkDestroyFence(g.device, g.present_fence, nullptr);
		vkDestroyCommandPool(g.device, g.pool, nullptr);
	}
	// Session before device: the runtime's session holds the device.
	if (g.session != XR_NULL_HANDLE) {
		xrDestroySession(g.session);
	}
	if (g.device != VK_NULL_HANDLE) {
		vkDestroyDevice(g.device, nullptr);
	}
	if (g.surface != VK_NULL_HANDLE) {
		vkDestroySurfaceKHR(g.vk_instance, g.surface, nullptr);
	}
	if (g.vk_instance != VK_NULL_HANDLE) {
		vkDestroyInstance(g.vk_instance, nullptr);
	}
	if (g.instance != XR_NULL_HANDLE) {
		xrDestroyInstance(g.instance);
	}
	g.wl_geom.disconnect();
	// The window LAST: the VkSurface borrowed its connection.
	g.window.destroy();
}

/*
 * --lattice-selftest (#1723): one title-bar press's drag-lattice table, built
 * exactly as displayxr-common's DxrLinuxWindow::wl_probe_lattice builds it on
 * its worker — dxr_wl_lattice::probe_via_grid over the installed grid
 * provider, the per-point provider for anything the grids did not answer —
 * against the per-point build (dxr_wl_lattice::probe). The adapter below is
 * that function's, with call counters; wl_probe_lattice itself is private.
 */
static bool
lattice_selftest()
{
	// displayxr-common dxr_linux_window.cpp kLatticeHalf / kLatticeCell.
	constexpr int32_t kHalf = 192, kCell = 3;
	if (!g.weave_snap.available() || !g.weave_snap.grid_available()) {
		LOGE("lattice selftest: xrWeaveSnapWindowRectDXR %s, xrWeaveSnapWindowGridDXR %s — nothing to test",
		     g.weave_snap.available() ? "resolved" : "MISSING",
		     g.weave_snap.grid_available() ? "resolved" : "MISSING");
		return false;
	}
	struct Case
	{
		const char *name;
		double scale;
		int32_t rel0_x, rel0_y;
		bool gate_calls; //!< the <= 3 calls bar applies
	};
	const Case cases[] = {
	    {"100 %", 1.0, 0, 0, true},
	    {"200 %", 2.0, 40, 25, true},
	    {"150 %", 1.5, 37, 11, false},
	};
	auto ms_since = [](std::chrono::steady_clock::time_point t0) {
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	};
	bool pass = true;
	for (const Case &c : cases) {
		dxr_wl_lattice::Map map;
		map.scale = c.scale;
		map.rel0_x = c.rel0_x;
		map.rel0_y = c.rel0_y;

		uint64_t point_calls = 0, grid_calls = 0;
		dxr_wl_lattice::PointSnapFn point = [&point_calls](int32_t tx, int32_t ty, int32_t *ox, int32_t *oy) {
			point_calls++;
			return DxrWeaveSnap::callback(&g.weave_snap, 0, 0, tx, ty, ox, oy);
		};
		std::vector<DxrLinuxWindow::SnapGridPoint> tmp;
		dxr_wl_lattice::GridSnapFn grid = [&grid_calls, &tmp](const dxr_wl_lattice::GridSpec &gs,
		                                                      dxr_wl_lattice::GridPoint *out, bool *declined) {
			const size_t n = (size_t)gs.count_x * gs.count_y;
			tmp.assign(n, DxrLinuxWindow::SnapGridPoint{0, 0});
			*declined = false;
			grid_calls++;
			if (!DxrWeaveSnap::grid_callback(&g.weave_snap, 0, 0, gs.first_x, gs.first_y, gs.step_x, gs.step_y,
			                                 gs.count_x, gs.count_y, tmp.data(), declined)) {
				return false;
			}
			for (size_t k = 0; k < n; k++) {
				const bool none = tmp[k].dx == DxrLinuxWindow::kSnapGridNoAnswer;
				out[k].dx = none ? dxr_wl_lattice::kGridNoAnswer : tmp[k].dx;
				out[k].dy = none ? dxr_wl_lattice::kGridNoAnswer : tmp[k].dy;
			}
			return true;
		};

		auto t0 = std::chrono::steady_clock::now();
		const dxr_wl_lattice::Probe want = dxr_wl_lattice::probe(point, map, 0, 0, kHalf, kCell);
		const double want_ms = ms_since(t0);
		const uint64_t want_calls = point_calls;

		point_calls = 0;
		t0 = std::chrono::steady_clock::now();
		const dxr_wl_lattice::Probe got = dxr_wl_lattice::probe_via_grid(grid, point, map, 0, 0, kHalf, kCell);
		const double got_ms = ms_since(t0);
		const uint64_t ipc = grid_calls + point_calls;

		const bool same = want.dxs == got.dxs && want.dys == got.dys && want.declined == got.declined;
		const bool ok = same && !want.declined && got.grid_used && (!c.gate_calls || ipc <= 3);
		LOGI("lattice selftest %s: per-point %llu IPC calls %.1f ms | grid provider %llu IPC call(s) (%llu grid, "
		     "%llu points, %llu single) %.2f ms | %zu entries, %zu probed, identical=%d%s%s%s -> %s",
		     c.name, (unsigned long long)want_calls, want_ms, (unsigned long long)ipc,
		     (unsigned long long)grid_calls, (unsigned long long)got.grid_points,
		     (unsigned long long)point_calls, got_ms, got.dxs.size(), got.probed, (int)same,
		     want.declined ? ", DP DECLINED" : "", got.grid_fallback != nullptr ? " | grid fallback: " : "",
		     got.grid_fallback != nullptr ? got.grid_fallback : "",
		     ok ? "OK" : "FAIL");
		pass = pass && ok;
	}
	LOGI("lattice selftest: %s", pass ? "PASS" : "FAIL");
	return pass;
}

int
main(int argc, char **argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	if (!parse_args(argc, argv, g.opt)) {
		return 2;
	}
	g.headless = g.opt.headless_frames > 0;
	if (g.headless) {
		g.opt.width = 1280;
		g.opt.height = 720;
		g.opt.anaglyph_check = false;
	}
	LOGI("=== weave_present_vk_linux: XR_DXR_weave present-owner (%s) ===",
	     g.headless ? "headless self-check" : "windowed");

	if (!init_openxr()) {
		cleanup();
		return 1;
	}

	if (!g.headless) {
		// The runtime is not this window's presenter, so both backends are
		// available regardless of which binding extensions it advertises.
		std::string reason;
		DxrWindowBackend be = DxrLinuxWindow::select(g.opt.backend, true, true, &reason);
		if (be == DxrWindowBackend::Auto) {
			LOGE("no usable window backend: %s", reason.c_str());
			cleanup();
			return 1;
		}
		LOGI("window backend: %s%s%s", DxrLinuxWindow::backend_name(be), reason.empty() ? "" : " — ",
		     reason.c_str());
		DxrLinuxWindowDesc desc;
		desc.width = g.opt.width;
		desc.height = g.opt.height;
		desc.panel_left = g.panel_left;
		desc.panel_top = g.panel_top;
		desc.panel_width = g.panel_w;
		desc.panel_height = g.panel_h;
		desc.title = "Weave Present VK (DisplayXR)";
		desc.app_id = "com.displayxr.weave_present_vk_linux";
		desc.fullscreen_on_wayland = g.opt.fullscreen;
		desc.x11_header_bar = true; // the bar drags (phase-snapped); the content is the scene
		desc.x11_drag_button = 0;
		if (!g.window.create(be, desc)) {
			LOGE("window creation failed");
			cleanup();
			return 1;
		}
		if (g.opt.fullscreen && be == DxrWindowBackend::X11) {
			g.window.toggle_fullscreen();
		}
		LOGI("window: %s (%s)", g.window.describe().c_str(), g.window.connection_description().c_str());
		if (be == DxrWindowBackend::Wayland) {
			g.wl_geom_ok = g.wl_geom.connect();
		}
	}

	if (!init_vulkan() || !init_session() || !init_pipeline()) {
		cleanup();
		return 1;
	}

	// Drag-time snap: the helper's drag (X11 every step, Wayland the drag
	// lattice) asks the display processor through xrWeaveSnapWindowRectDXR —
	// served by the SERVICE's DP here, since this session is a present-owner.
	// The Wayland lattice table (16,641+ points per press) is built by the
	// helper on its worker from a few xrWeaveSnapWindowGridDXR calls through
	// the grid provider (#1723): over IPC the per-point probe was one round
	// trip per point, 2.3-2.5 s a press.
	g.weave_snap.attach(g.instance, g.session, g.opt.width, g.opt.height);
	if (!g.headless) {
		g.window.set_snap_provider(&DxrWeaveSnap::callback, &g.weave_snap);
		if (g.weave_snap.grid_available()) {
			g.window.set_snap_grid_provider(&DxrWeaveSnap::grid_callback, &g.weave_snap);
		}
		LOGI("drag snap: xrWeaveSnapWindowRectDXR %s, grid snap (v11) %s",
		     g.weave_snap.available() ? "RESOLVED" : "unavailable",
		     g.weave_snap.grid_available() ? "RESOLVED — a few calls per lattice table, on the helper's worker"
		                                   : "unavailable — the lattice is probed point by point");
		if (!create_surface() || !create_swapchain()) {
			cleanup();
			return 1;
		}
	} else if (!create_content(g.opt.width, g.opt.height)) {
		cleanup();
		return 1;
	}

	int rc = run();
	// After the frames: the service's weave engine (and so its display
	// processor) exists only once this present-owner has submitted, and a
	// snap before that is declined.
	const bool lattice_ok = !g.opt.lattice_selftest || lattice_selftest();
	cleanup();
	if (!lattice_ok) {
		LOGE("--lattice-selftest FAILED");
		rc = rc != 0 ? rc : 1;
	}
	return rc;
}
