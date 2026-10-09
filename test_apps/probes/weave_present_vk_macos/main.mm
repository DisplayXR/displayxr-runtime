// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_weave macOS on-panel presenter (#759) — eyeball a weave backend
 *         on a real panel without the browser.
 *
 * A windowed present-owner built on the same forced-IPC, headless-MoltenVK
 * session as weave_probe_vk_macos:
 *
 *   1. Opens a titled, movable, resizable NSWindow on --display <CGDirectDisplayID>
 *      (default: the first non-main display, else the main one).
 *   2. Every frame CPU-fills a window-client-sized (backing px) global BGRA
 *      IOSurface with a squeezed 2x1 SBS stereo test pattern (left half = left
 *      eye, right half = right eye): a red box at +20 px crossed disparity (in
 *      front), a cyan box at -20 px (behind), a white box and a 1-px vertical
 *      line grid at zero disparity (a phase error shows as moire on the grid),
 *      plus a frame-counter / sweeping-marker band so animation is visible.
 *   3. Sends the client-area geometry (global CoreGraphics BACKING px, y down,
 *      displayId = CGDirectDisplayID — spec §5) via xrWeaveBindWindow2DXR on the
 *      first frame and whenever the window moves / resizes / changes screen
 *      (not literally every frame: every bind logs a service WARN).
 *   4. xrWeaveSubmitDXR with ONE XrWeaveSubmitRectsDXR rect = the whole client
 *      area (the spec-v3 batch contract, the browser's path). On both macOS
 *      backends that un-squeezes the rect into a 2W x H scratch and runs ONE
 *      process_atlas with a 2x1 tiling — exactly what a Leia DP consumes — and
 *      the woven output is window-sized on every DP (v6 would hand back a
 *      content-view-sized output on a resample-tolerant DP such as sim_display).
 *   5. Shows the woven IOSurface 1:1 as a plain CALayer's contents
 *      (contentsScale = backing scale, top-left gravity, nearest filters), with
 *      the window and the IOSurface tagged with CGDisplayCopyColorSpace(the
 *      window's display) so WindowServer applies no colour matching. The runtime
 *      re-hands the SAME IOSurfaceRef every frame; CA caches by identity, so each
 *      frame re-sets contents (nil, then the surface) in an action-less
 *      CATransaction. --ring copies into a 3-slot client ring instead (the
 *      browser's approach) if the plain path ever freezes.
 *
 * Flags: --display N  --size WxH (points, default 1280x720)  --secs N (auto-quit)
 *        --sbs-only (present the raw SBS input, no weave)  --ring
 *
 * Run: see run_weave_present_leia.sh next to this file (service + presenter, sim or
 * Leia), or with a service already up:
 *   XRT_FORCE_MODE=ipc XR_RUNTIME_JSON=_package/DisplayXR-macOS/openxr_displayxr.json \
 *     DYLD_LIBRARY_PATH=_package/DisplayXR-macOS/lib ./weave_present_vk_macos --display 2
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>

#include <vulkan/vulkan.h>

#define XR_USE_PLATFORM_MACOS 1
#define XR_USE_GRAPHICS_API_VULKAN 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_DXR_weave.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define LOG(...)                                                                                                       \
	do {                                                                                                           \
		fprintf(stderr, "[weave_present] " __VA_ARGS__);                                                       \
		fprintf(stderr, "\n");                                                                                 \
	} while (0)

/*
 *
 * Options + state.
 *
 */

struct Options
{
	uint32_t display = 0; // 0 = auto
	int width_pt = 1280, height_pt = 720;
	double secs = 0;
	bool sbs_only = false;
	bool ring = false;
};

struct Xr
{
	XrInstance instance = XR_NULL_HANDLE;
	XrSession session = XR_NULL_HANDLE;
	VkInstance vk_instance = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	PFN_xrWeaveBindWindow2DXR bind2 = nullptr;
	PFN_xrWeaveSubmitDXR submit = nullptr;
};

static Options g_opt;
static Xr g_xr;

/*
 *
 * Headless Vulkan + forced-IPC session (as weave_probe_vk_macos).
 *
 */

static void
split_exts(const std::string &s, std::vector<std::string> &storage, std::vector<const char *> &ptrs)
{
	size_t start = 0;
	for (size_t i = 0; i <= s.size(); i++) {
		if (i == s.size() || s[i] == ' ' || s[i] == '\0') {
			if (i > start) {
				storage.push_back(s.substr(start, i - start));
			}
			start = i + 1;
		}
	}
	for (const auto &e : storage) {
		ptrs.push_back(e.c_str());
	}
}

#define XRB(call)                                                                                                      \
	do {                                                                                                           \
		XrResult _r = (call);                                                                                  \
		if (XR_FAILED(_r)) {                                                                                   \
			LOG("FAILED %s -> %d", #call, (int)_r);                                                        \
			return false;                                                                                  \
		}                                                                                                      \
	} while (0)
#define VKB(call)                                                                                                      \
	do {                                                                                                           \
		VkResult _v = (call);                                                                                  \
		if (_v != VK_SUCCESS) {                                                                                \
			LOG("FAILED %s -> %d", #call, (int)_v);                                                        \
			return false;                                                                                  \
		}                                                                                                      \
	} while (0)

static bool
has_ext(const std::vector<VkExtensionProperties> &v, const char *name)
{
	for (const auto &e : v) {
		if (strcmp(e.extensionName, name) == 0) {
			return true;
		}
	}
	return false;
}

static bool
xr_init(Xr &xr)
{
	const char *enabled[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_DXR_WEAVE_EXTENSION_NAME};
	XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
	snprintf(ici.applicationInfo.applicationName, sizeof(ici.applicationInfo.applicationName), "%s",
	         "DXRWeavePresentMacOS");
	ici.applicationInfo.applicationVersion = 1;
	snprintf(ici.applicationInfo.engineName, sizeof(ici.applicationInfo.engineName), "%s", "None");
	ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	ici.enabledExtensionCount = 2;
	ici.enabledExtensionNames = enabled;
	XRB(xrCreateInstance(&ici, &xr.instance));

	XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId sys = XR_NULL_SYSTEM_ID;
	XRB(xrGetSystem(xr.instance, &sgi, &sys));

	PFN_xrGetVulkanGraphicsRequirementsKHR pfn_req = nullptr;
	PFN_xrGetVulkanInstanceExtensionsKHR pfn_iext = nullptr;
	PFN_xrGetVulkanDeviceExtensionsKHR pfn_dext = nullptr;
	PFN_xrGetVulkanGraphicsDeviceKHR pfn_gdev = nullptr;
	xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&pfn_req);
	xrGetInstanceProcAddr(xr.instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction *)&pfn_iext);
	xrGetInstanceProcAddr(xr.instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction *)&pfn_dext);
	xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction *)&pfn_gdev);
	xrGetInstanceProcAddr(xr.instance, "xrWeaveBindWindow2DXR", (PFN_xrVoidFunction *)&xr.bind2);
	xrGetInstanceProcAddr(xr.instance, "xrWeaveSubmitDXR", (PFN_xrVoidFunction *)&xr.submit);
	if (!pfn_req || !pfn_iext || !pfn_dext || !pfn_gdev || !xr.bind2 || !xr.submit) {
		LOG("failed to resolve XR_KHR_vulkan_enable / XR_DXR_weave entry points");
		return false;
	}
	XrGraphicsRequirementsVulkanKHR vk_req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
	XRB(pfn_req(xr.instance, sys, &vk_req));

	uint32_t len = 0;
	pfn_iext(xr.instance, sys, 0, &len, nullptr);
	std::string iext_str(len, '\0');
	pfn_iext(xr.instance, sys, len, &len, iext_str.data());
	std::vector<std::string> iext_storage;
	std::vector<const char *> iexts;
	split_exts(iext_str, iext_storage, iexts);
	uint32_t avail = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &avail, nullptr);
	std::vector<VkExtensionProperties> avail_exts(avail);
	vkEnumerateInstanceExtensionProperties(nullptr, &avail, avail_exts.data());
	const bool portability = has_ext(avail_exts, "VK_KHR_portability_enumeration");
	if (portability) {
		iexts.push_back("VK_KHR_portability_enumeration");
	}

	VkApplicationInfo app_info = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app_info.pApplicationName = "DXRWeavePresentMacOS";
	app_info.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo vici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	vici.pApplicationInfo = &app_info;
	vici.enabledExtensionCount = (uint32_t)iexts.size();
	vici.ppEnabledExtensionNames = iexts.data();
	if (portability) {
		vici.flags |= 0x00000001; // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
	}
	VKB(vkCreateInstance(&vici, nullptr, &xr.vk_instance));

	VkPhysicalDevice phys = VK_NULL_HANDLE;
	XRB(pfn_gdev(xr.instance, sys, xr.vk_instance, &phys));
	uint32_t qf_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, nullptr);
	std::vector<VkQueueFamilyProperties> qfs(qf_count);
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, qfs.data());
	uint32_t qfi = UINT32_MAX;
	for (uint32_t i = 0; i < qf_count && qfi == UINT32_MAX; i++) {
		if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			qfi = i;
		}
	}
	if (qfi == UINT32_MAX) {
		LOG("no graphics queue family");
		return false;
	}

	len = 0;
	pfn_dext(xr.instance, sys, 0, &len, nullptr);
	std::string dext_str(len, '\0');
	pfn_dext(xr.instance, sys, len, &len, dext_str.data());
	std::vector<std::string> dext_storage;
	std::vector<const char *> dexts;
	split_exts(dext_str, dext_storage, dexts);
	uint32_t dav = 0;
	vkEnumerateDeviceExtensionProperties(phys, nullptr, &dav, nullptr);
	std::vector<VkExtensionProperties> dav_exts(dav);
	vkEnumerateDeviceExtensionProperties(phys, nullptr, &dav, dav_exts.data());
	if (has_ext(dav_exts, "VK_KHR_portability_subset")) {
		bool already = false;
		for (const char *d : dexts) {
			already = already || strcmp(d, "VK_KHR_portability_subset") == 0;
		}
		if (!already) {
			dexts.push_back("VK_KHR_portability_subset");
		}
	}

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = qfi;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = (uint32_t)dexts.size();
	dci.ppEnabledExtensionNames = dexts.data();
	VKB(vkCreateDevice(phys, &dci, nullptr, &xr.device));

	XrGraphicsBindingVulkanKHR vk_binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
	vk_binding.instance = xr.vk_instance;
	vk_binding.physicalDevice = phys;
	vk_binding.device = xr.device;
	vk_binding.queueFamilyIndex = qfi;
	vk_binding.queueIndex = 0;
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &vk_binding;
	sci.systemId = sys;
	XRB(xrCreateSession(xr.instance, &sci, &xr.session));
	LOG("session created (forced IPC, headless Vulkan)");
	return true;
}

static void
xr_shutdown(Xr &xr)
{
	if (xr.session != XR_NULL_HANDLE) {
		xrDestroySession(xr.session);
	}
	if (xr.instance != XR_NULL_HANDLE) {
		xrDestroyInstance(xr.instance);
	}
	if (xr.device != VK_NULL_HANDLE) {
		vkDestroyDevice(xr.device, nullptr);
	}
	if (xr.vk_instance != VK_NULL_HANDLE) {
		vkDestroyInstance(xr.vk_instance, nullptr);
	}
	xr = Xr{};
}

/*
 *
 * IOSurfaces + the stereo test pattern.
 *
 */

//! Global BGRA8 IOSurface (kIOSurfaceIsGlobal: the IPC carries the bare IOSurfaceID).
static IOSurfaceRef
create_bgra_surface(int32_t w, int32_t h)
{
	int32_t bpe = 4;
	uint32_t fmt = 'BGRA';
	CFStringRef keys[5] = {CFSTR("IOSurfaceWidth"), CFSTR("IOSurfaceHeight"), CFSTR("IOSurfaceBytesPerElement"),
	                       CFSTR("IOSurfacePixelFormat"), CFSTR("IOSurfaceIsGlobal")};
	CFNumberRef nums[4] = {CFNumberCreate(nullptr, kCFNumberSInt32Type, &w),
	                       CFNumberCreate(nullptr, kCFNumberSInt32Type, &h),
	                       CFNumberCreate(nullptr, kCFNumberSInt32Type, &bpe),
	                       CFNumberCreate(nullptr, kCFNumberSInt32Type, &fmt)};
	CFTypeRef vals[5] = {nums[0], nums[1], nums[2], nums[3], kCFBooleanTrue};
	CFDictionaryRef props = CFDictionaryCreate(nullptr, (const void **)keys, (const void **)vals, 5,
	                                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	IOSurfaceRef s = IOSurfaceCreate(props);
	CFRelease(props);
	for (CFNumberRef n : nums) {
		CFRelease(n);
	}
	return s;
}

//! Tag an IOSurface with a display's colour space (CA then does no matching).
static void
tag_surface(IOSurfaceRef s, CGDirectDisplayID did)
{
	CGColorSpaceRef cs = CGDisplayCopyColorSpace(did);
	if (cs == nullptr) {
		return;
	}
	CFPropertyListRef plist = CGColorSpaceCopyPropertyList(cs);
	if (plist != nullptr) {
		IOSurfaceSetValue(s, CFSTR("IOSurfaceColorSpace"), plist);
		CFRelease(plist);
	}
	CGColorSpaceRelease(cs);
}

struct Rgb
{
	uint8_t r, g, b;
};

//! Squeezed-SBS canvas: W x H BGRA, eye e owns columns [e*W/2, (e+1)*W/2).
struct Canvas
{
	uint8_t *px;
	size_t stride;
	int w, h;

	void
	put(int x, int y, Rgb c)
	{
		uint8_t *p = px + (size_t)y * stride + (size_t)x * 4;
		p[0] = c.b;
		p[1] = c.g;
		p[2] = c.r;
		p[3] = 0xFF;
	}
	//! Fill a rect given in HALF-width (squeezed) coords of eye @p eye, clipped to that half.
	void
	eye_rect(int eye, int hx, int y, int rw, int rh, Rgb c)
	{
		const int half = w / 2, x0 = eye * half;
		for (int yy = std::max(y, 0); yy < std::min(y + rh, h); yy++) {
			for (int xx = std::max(hx, 0); xx < std::min(hx + rw, half); xx++) {
				put(x0 + xx, yy, c);
			}
		}
	}
	//! The same object in both eyes; @p disp_view_px = x_left - x_right in UN-squeezed view px
	//! (> 0 = crossed = in front of the panel). Squeezed, each eye moves by disp/4.
	void
	stereo_rect(int hx, int y, int rw, int rh, int disp_view_px, Rgb c)
	{
		const int d = disp_view_px / 4;
		eye_rect(0, hx + d, y, rw, rh, c);
		eye_rect(1, hx - d, y, rw, rh, c);
	}
};

// 3x5 digit glyphs, bit 2 = left column.
static const uint8_t kGlyph[10][5] = {{7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7},
                                      {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1},
                                      {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}};

static const Rgb kBg = {24, 24, 24}, kGrid = {96, 96, 96}, kWhite = {240, 240, 240}, kRed = {255, 40, 40},
                 kCyan = {40, 230, 255}, kYellow = {255, 220, 0};

//! Static scene (no frame-dependent content).
static void
draw_static(Canvas c)
{
	const int half = c.w / 2;
	for (int y = 0; y < c.h; y++) {
		for (int x = 0; x < c.w; x++) {
			c.put(x, y, kBg);
		}
	}
	// Zero-disparity 1-px line grid every 16 squeezed px (= 32 view px).
	for (int e = 0; e < 2; e++) {
		for (int hx = 0; hx < half; hx += 16) {
			c.eye_rect(e, hx, 0, 1, c.h, kGrid);
		}
	}
	const int bw = half / 6, bh = c.h / 4, by = c.h / 4;
	c.stereo_rect(half / 8, by, bw, bh, +20, kRed);                 // in front
	c.stereo_rect(half / 2 - bw / 2, by, bw, bh, 0, kWhite);        // on the panel plane
	c.stereo_rect(half - half / 8 - bw, by, bw, bh, -20, kCyan);    // behind
}

//! Dynamic band [band_y, band_y + band_h): frame digits + a sweeping marker, zero disparity.
static void
band_geometry(int h, int *band_y, int *band_h)
{
	*band_h = std::max(h / 8, 30);
	*band_y = h - *band_h - h / 12;
}

static void
draw_dynamic(Canvas c, uint64_t frame)
{
	int band_y = 0, band_h = 0;
	band_geometry(c.h, &band_y, &band_h);
	const int half = c.w / 2;
	const int cell = std::max(band_h / 6, 2); // glyph pixel size (squeezed x == view-y scale / 2)
	char digits[24];
	snprintf(digits, sizeof(digits), "%06llu", (unsigned long long)(frame % 1000000));
	int hx = half / 8;
	for (const char *p = digits; *p; p++) {
		const uint8_t *g = kGlyph[*p - '0'];
		for (int row = 0; row < 5; row++) {
			for (int col = 0; col < 3; col++) {
				if (g[row] & (4 >> col)) {
					c.stereo_rect(hx + col * cell / 2, band_y + row * cell, cell / 2 + 1, cell, 0,
					              kWhite);
				}
			}
		}
		hx += 2 * cell;
	}
	// Marker sweeps the right part of the band once per ~2 s at 60 fps.
	const int span = half / 2, mw = std::max(half / 64, 2);
	const int mx = half / 2 - half / 16 + (int)((frame * 4) % (uint64_t)std::max(span, 1));
	c.stereo_rect(mx, band_y, mw, band_h, 0, kYellow);
}

/*
 *
 * The presenter.
 *
 */

@interface Presenter : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation Presenter
{
	NSWindow *_win;
	CALayer *_layer;
	NSTimer *_timer;
	IOSurfaceRef _input, _woven, _ring[3];
	std::vector<uint8_t> _base; // static scene, tight stride
	int _w, _h;                 // input = client area, backing px
	uint64_t _frame;
	int _ring_i;
	CGDirectDisplayID _tagged_did;
	bool _have_geom;
	XrOffset2Di _geom_origin;
	XrExtent2Di _geom_size;
	int32_t _geom_did;
	uint32_t _geom_sends, _out_w, _out_h, _fail;
	double _stat_t0;
	uint64_t _stat_frames;
	XrWeaveOutputDXR _last_out;
}

static double
now_s()
{
	return CACurrentMediaTime();
}

static CGDirectDisplayID
screen_did(NSScreen *s)
{
	return s ? [[[s deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue] : 0;
}

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
	CGDirectDisplayID want = g_opt.display;
	NSScreen *target = nil;
	for (NSScreen *s in [NSScreen screens]) {
		if (want == 0 && screen_did(s) != CGMainDisplayID()) {
			target = s;
			break;
		}
		if (want != 0 && screen_did(s) == want) {
			target = s;
		}
	}
	if (target == nil) {
		if (want != 0) {
			LOG("no screen with CGDirectDisplayID %u", want);
			[NSApp terminate:nil];
			return;
		}
		target = [NSScreen mainScreen];
	}
	NSRect vf = target.visibleFrame;
	NSSize sz = NSMakeSize(std::min<CGFloat>(g_opt.width_pt, vf.size.width - 40),
	                       std::min<CGFloat>(g_opt.height_pt, vf.size.height - 60));
	NSRect cr = NSMakeRect(NSMidX(vf) - sz.width / 2, NSMidY(vf) - sz.height / 2, sz.width, sz.height);
	_win = [[NSWindow alloc] initWithContentRect:cr
	                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
	                                             NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
	                                     backing:NSBackingStoreBuffered
	                                       defer:NO
	                                      screen:target];
	_win.releasedWhenClosed = NO;
	_win.delegate = self;
	_win.title = [NSString stringWithFormat:@"DisplayXR weave presenter%s (display %u)",
	                                        g_opt.sbs_only ? " — RAW SBS" : "", screen_did(target)];
	NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, sz.width, sz.height)];
	_layer = [CALayer layer];
	_layer.contentsGravity = kCAGravityTopLeft;
	_layer.magnificationFilter = kCAFilterNearest;
	_layer.minificationFilter = kCAFilterNearest;
	_layer.opaque = YES;
	_layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
	view.layer = _layer; // layer-hosting
	view.wantsLayer = YES;
	_win.contentView = view;
	[_win setFrame:[_win frameRectForContentRect:cr] display:NO];
	[_win orderFrontRegardless]; // no activation: never steal focus

	if (!xr_init(g_xr)) {
		[NSApp terminate:nil];
		return;
	}
	_stat_t0 = now_s();
	_timer = [NSTimer timerWithTimeInterval:1.0 / 60.0 target:self selector:@selector(tick) userInfo:nil repeats:YES];
	[[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes]; // keeps ticking during drags
	if (g_opt.secs > 0) {
		[NSTimer scheduledTimerWithTimeInterval:g_opt.secs
		                                repeats:NO
		                                  block:^(NSTimer *t) {
			                                  LOG("--secs elapsed, quitting");
			                                  [NSApp terminate:nil];
		                                  }];
	}
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a
{
	return YES;
}

- (void)applicationWillTerminate:(NSNotification *)n
{
	[_timer invalidate];
	_layer.contents = nil;
	xr_shutdown(g_xr);
	for (IOSurfaceRef *s : {&_input, &_woven, &_ring[0], &_ring[1], &_ring[2]}) {
		if (*s) {
			CFRelease(*s);
			*s = nullptr;
		}
	}
	LOG("exit after %llu frames (%u submit failures)", (unsigned long long)_frame, _fail);
}

//! (Re)allocate the input surface + static scene when the client area's backing size changes.
- (bool)ensureInput:(int)w h:(int)h
{
	if (_input && w == _w && h == _h) {
		return true;
	}
	if (_input) {
		CFRelease(_input);
	}
	_input = create_bgra_surface(w, h);
	if (!_input) {
		LOG("IOSurfaceCreate %dx%d failed", w, h);
		return false;
	}
	_w = w;
	_h = h;
	_tagged_did = 0;
	_base.assign((size_t)w * h * 4, 0);
	draw_static(Canvas{_base.data(), (size_t)w * 4, w, h});
	IOSurfaceLock(_input, 0, nullptr);
	uint8_t *dst = (uint8_t *)IOSurfaceGetBaseAddress(_input);
	size_t stride = IOSurfaceGetBytesPerRow(_input);
	for (int y = 0; y < h; y++) {
		memcpy(dst + (size_t)y * stride, _base.data() + (size_t)y * w * 4, (size_t)w * 4);
	}
	IOSurfaceUnlock(_input, 0, nullptr);
	LOG("input SBS surface %dx%d backing px (IOSurfaceID %u)", w, h, IOSurfaceGetID(_input));
	return true;
}

//! Send the client-area geometry (spec §5 units) when it changed.
- (void)sendGeometryIfChanged:(CGDirectDisplayID)did
{
	NSRect content = [_win contentRectForFrameRect:_win.frame]; // Cocoa global points, y up
	const CGFloat primary_h = [NSScreen screens][0].frame.size.height;
	const CGFloat s = _win.backingScaleFactor;
	XrOffset2Di o = {(int32_t)lround(content.origin.x * s),
	                 (int32_t)lround((primary_h - (content.origin.y + content.size.height)) * s)};
	XrExtent2Di e = {_w, _h};
	if (_have_geom && o.x == _geom_origin.x && o.y == _geom_origin.y && e.width == _geom_size.width &&
	    e.height == _geom_size.height && (int32_t)did == _geom_did) {
		return;
	}
	XrWeaveWindowGeometryDXR geom = {(XrStructureType)XR_TYPE_WEAVE_WINDOW_GEOMETRY_DXR};
	geom.windowOriginOnScreen = o;
	geom.clientSize = e;
	geom.displayId = (int32_t)did;
	XrWeaveBindWindowInfoDXR bind = {(XrStructureType)XR_TYPE_WEAVE_BIND_WINDOW_INFO_DXR};
	bind.next = &geom;
	bind.windowHandle = (__bridge void *)_win; // opaque id on macOS
	XrResult r = g_xr.bind2(g_xr.session, &bind);
	if (XR_FAILED(r)) {
		LOG("xrWeaveBindWindow2DXR failed: %d", (int)r);
		return;
	}
	_have_geom = true;
	_geom_origin = o;
	_geom_size = e;
	_geom_did = (int32_t)did;
	_geom_sends++;
}

- (void)present:(IOSurfaceRef)surf
{
	[CATransaction begin];
	[CATransaction setDisableActions:YES];
	_layer.contentsScale = _win.backingScaleFactor;
	_layer.contents = nil; // CA caches by surface identity: force a re-read of the same IOSurfaceRef
	_layer.contents = (__bridge id)surf;
	[CATransaction commit];
}

- (void)tick
{
	if (g_xr.session == XR_NULL_HANDLE) {
		return;
	}
	NSView *view = _win.contentView;
	NSSize bs = [view convertRectToBacking:view.bounds].size;
	const CGDirectDisplayID did = screen_did(_win.screen);
	if (![self ensureInput:(int)lround(bs.width) h:(int)lround(bs.height)]) {
		return;
	}
	if (did != _tagged_did && did != 0) {
		CGColorSpaceRef cs = CGDisplayCopyColorSpace(did);
		if (cs) {
			_win.colorSpace = [[NSColorSpace alloc] initWithCGColorSpace:cs];
			CGColorSpaceRelease(cs);
		}
		tag_surface(_input, did);
		if (_woven) {
			tag_surface(_woven, did);
		}
		for (IOSurfaceRef r : _ring) {
			if (r) {
				tag_surface(r, did);
			}
		}
		_tagged_did = did;
		LOG("window + surfaces tagged with the colour space of display %u", did);
	}

	// Dynamic band: restore from the static scene, then draw this frame's counter + marker.
	int band_y = 0, band_h = 0;
	band_geometry(_h, &band_y, &band_h);
	IOSurfaceLock(_input, 0, nullptr);
	uint8_t *dst = (uint8_t *)IOSurfaceGetBaseAddress(_input);
	size_t stride = IOSurfaceGetBytesPerRow(_input);
	for (int y = band_y; y < band_y + band_h && y < _h; y++) {
		memcpy(dst + (size_t)y * stride, _base.data() + (size_t)y * _w * 4, (size_t)_w * 4);
	}
	draw_dynamic(Canvas{dst, stride, _w, _h}, _frame);
	IOSurfaceUnlock(_input, 0, nullptr);

	IOSurfaceRef show = _input;
	if (!g_opt.sbs_only) {
		[self sendGeometryIfChanged:did];
		XrRect2Di whole = {{0, 0}, {_w, _h}};
		XrWeaveSubmitRectsDXR rects = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_RECTS_DXR};
		rects.rectCount = 1;
		rects.rects = &whole;
		XrWeaveSubmitInfoDXR submit = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_INFO_DXR};
		submit.next = &rects;
		submit.inputTexture = (void *)_input;
		submit.inputIsDxgi = XR_FALSE;
		submit.rect = whole;
		submit.firstChunk = XR_TRUE;
		XrWeaveOutputDXR out = {(XrStructureType)XR_TYPE_WEAVE_OUTPUT_DXR};
		XrResult r = g_xr.submit(g_xr.session, &submit, &out);
		if (XR_FAILED(r)) {
			if (_fail++ < 5) {
				LOG("xrWeaveSubmitDXR failed: %d", (int)r);
			}
			if (r == XR_ERROR_INSTANCE_LOST || r == XR_ERROR_SESSION_LOST) {
				[NSApp terminate:nil];
			}
			return;
		}
		if (out.weavedTexture != nullptr) { // first submit + every re-allocation (resize)
			if (_woven) {
				CFRelease(_woven);
			}
			_woven = (IOSurfaceRef)out.weavedTexture; // retained handback, ours to release
			tag_surface(_woven, did);
			LOG("woven output handed back: %ux%u (IOSurfaceID %u)", out.width, out.height,
			    IOSurfaceGetID(_woven));
		}
		_out_w = out.width;
		_out_h = out.height;
		_last_out = out;
		show = _woven;
	}
	if (show && g_opt.ring) {
		// The browser's 3-slot ring: a distinct IOSurface every frame so CA must re-read.
		const int w = (int)IOSurfaceGetWidth(show), h = (int)IOSurfaceGetHeight(show);
		IOSurfaceRef &slot = _ring[_ring_i];
		_ring_i = (_ring_i + 1) % 3;
		if (slot && ((int)IOSurfaceGetWidth(slot) != w || (int)IOSurfaceGetHeight(slot) != h)) {
			CFRelease(slot);
			slot = nullptr;
		}
		if (!slot) {
			slot = create_bgra_surface(w, h);
			tag_surface(slot, did);
		}
		IOSurfaceLock(show, kIOSurfaceLockReadOnly, nullptr);
		IOSurfaceLock(slot, 0, nullptr);
		const size_t ss = IOSurfaceGetBytesPerRow(show), ds = IOSurfaceGetBytesPerRow(slot);
		for (int y = 0; y < h; y++) {
			memcpy((uint8_t *)IOSurfaceGetBaseAddress(slot) + y * ds,
			       (uint8_t *)IOSurfaceGetBaseAddress(show) + y * ss, (size_t)w * 4);
		}
		IOSurfaceUnlock(slot, 0, nullptr);
		IOSurfaceUnlock(show, kIOSurfaceLockReadOnly, nullptr);
		show = slot;
	}
	if (show) {
		[self present:show];
	}
	_frame++;
	_stat_frames++;

	const double t = now_s();
	if (t - _stat_t0 >= 1.0) {
		const XrWeaveOutputDXR &o = _last_out;
		char eyes[160] = "n/a";
		if (!g_opt.sbs_only && o.eyeCount >= 2) {
			snprintf(eyes, sizeof(eyes), "%s%s L(%.3f,%.3f,%.3f) R(%.3f,%.3f,%.3f)",
			         o.eyesValid ? "valid" : "INVALID", o.eyesTracking ? "+tracking" : "+NOT-tracking",
			         o.eyes[0].x, o.eyes[0].y, o.eyes[0].z, o.eyes[1].x, o.eyes[1].y, o.eyes[1].z);
		} else if (!g_opt.sbs_only) {
			snprintf(eyes, sizeof(eyes), "count=%u valid=%d", o.eyeCount, (int)o.eyesValid);
		}
		LOG("frame %llu  %.1f fps  input %dx%d  %s %ux%u  geom (%d,%d) %dx%d disp %d (sent %u)  eyes %s",
		    (unsigned long long)_frame, _stat_frames / (t - _stat_t0), _w, _h,
		    g_opt.sbs_only ? "RAW-SBS" : "woven", g_opt.sbs_only ? (unsigned)_w : _out_w,
		    g_opt.sbs_only ? (unsigned)_h : _out_h, _geom_origin.x, _geom_origin.y, _geom_size.width,
		    _geom_size.height, _geom_did, _geom_sends, eyes);
		_stat_t0 = t;
		_stat_frames = 0;
	}
}
@end

static void
usage(const char *argv0)
{
	LOG("usage: %s [--display <CGDirectDisplayID>] [--size WxH] [--secs N] [--sbs-only] [--ring]", argv0);
}

int
main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "--display") == 0 && i + 1 < argc) {
			g_opt.display = (uint32_t)strtoul(argv[++i], nullptr, 0);
		} else if (strcmp(a, "--size") == 0 && i + 1 < argc) {
			if (sscanf(argv[++i], "%dx%d", &g_opt.width_pt, &g_opt.height_pt) != 2 || g_opt.width_pt < 64 ||
			    g_opt.height_pt < 64) {
				usage(argv[0]);
				return 1;
			}
		} else if (strcmp(a, "--secs") == 0 && i + 1 < argc) {
			g_opt.secs = atof(argv[++i]);
		} else if (strcmp(a, "--sbs-only") == 0) {
			g_opt.sbs_only = true;
		} else if (strcmp(a, "--ring") == 0) {
			g_opt.ring = true;
		} else {
			usage(argv[0]);
			return 1;
		}
	}
	@autoreleasepool {
		NSApplication *app = [NSApplication sharedApplication];
		[app setActivationPolicy:NSApplicationActivationPolicyRegular];
		Presenter *p = [Presenter new];
		app.delegate = p;
		// SIGINT / SIGTERM -> orderly terminate (destroys the session; the run script relies on it).
		for (int sig : {SIGINT, SIGTERM}) {
			signal(sig, SIG_IGN);
			dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, sig, 0,
			                                               dispatch_get_main_queue());
			dispatch_source_set_event_handler(src, ^{
				[NSApp terminate:nil];
			});
			dispatch_resume(src);
			(void)CFBridgingRetain(src); // live for the process
		}
		[app run];
	}
	return 0;
}
