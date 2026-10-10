// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  colour_probe_metal_macos — the ADR-044 numerical oracle for the
 *         macOS Metal compositor.
 *
 * Every layer is a SOLID, CPU-authored image uploaded with -replaceRegion:,
 * which moves bytes without any conversion, so the bytes the compositor reads
 * are exactly the bytes written here whatever the declared format is. What the
 * atlas holds afterwards therefore depends ONLY on how the compositor reads the
 * declared format and where it blends:
 *
 *   projection (per view): left half byte 38, right half byte 200, opaque.
 *                          Format from DXR_PROBE_PROJ = srgb (default) | unorm.
 *   window-space layers (DXR_PROBE_WS=1, default):
 *     L1  _SRGB  opaque byte 38            rect (0.05,0.05)+(0.15,0.35)
 *     L2  UNORM  opaque byte 38            rect (0.25,0.05)+(0.15,0.35)
 *     L3  _SRGB  black, straight a=128     rect (0.60,0.05)+(0.30,0.35)  over 200
 *     L4  _SRGB  white, straight a=128     rect (0.05,0.50)+(0.40,0.35)  over 38
 *
 * Format-honest expectations (ADR-044 §1): an _SRGB 38 reads back 38, a UNORM
 * 38 reads back 108 (exactly one encode); L3 = 147 / L4 = 190 when the blend is
 * in linear light, 100 / 147 when it is in encoded space.
 *
 * After DXR_PROBE_FRAMES frames (default 90) the probe touches its PER-EXE
 * trigger, $TMPDIR/displayxr_atlas_trigger.colour_probe_metal_macos (#433:
 * never the global /tmp/dxr_atlas_trigger, which any other Metal or GL app on
 * the box races for), waits for $TMPDIR/displayxr_atlas.colour_probe_metal_macos.png
 * and exits; read_atlas.py beside this file samples the regions and prints
 * them. Run with DXR_ATLAS_CAPTURE_RAW_ALPHA=1 to read the true atlas alpha.
 * DXR_PROBE_TEXTURE=1 runs the shared-IOSurface (_texture) session shape
 * instead of a window. DXR_PROBE_TRANSPARENT=1 requests a transparent
 * background and makes the projection's right half (0,0,0,0): the base blit is
 * a REPLACE, so that alpha must reach the atlas verbatim (#225 / §10.6.2).
 *
 * The window is placed on the BUILT-IN display (CGDisplayIsBuiltin), never on
 * an external panel.
 */

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#define XR_USE_GRAPHICS_API_METAL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_DXR_cocoa_window_binding.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#define LOG(fmt, ...) fprintf(stderr, "[probe] " fmt "\n", ##__VA_ARGS__)

#define XR_OK(call)                                                                                                    \
	do {                                                                                                           \
		XrResult _r = (call);                                                                                  \
		if (XR_FAILED(_r)) {                                                                                   \
			LOG("OpenXR error %d at line %d: %s", (int)_r, __LINE__, #call);                              \
			return 1;                                                                                      \
		}                                                                                                      \
	} while (0)

static std::string g_trigger;
static std::string g_atlas;

struct WsLayer
{
	const char *name;
	bool srgb;
	uint8_t bgra[4];
	bool straight; // XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
	float x, y, w, h;
	XrSwapchain sc = XR_NULL_HANDLE;
};

static bool
env_is(const char *name, const char *value)
{
	const char *e = getenv(name);
	return e != nullptr && strcmp(e, value) == 0;
}

static int
env_int(const char *name, int def)
{
	const char *e = getenv(name);
	return (e != nullptr && *e != '\0') ? atoi(e) : def;
}

//! The built-in display's frame (AppKit coordinates), else the first screen.
static NSRect
builtin_screen_frame(void)
{
	for (NSScreen *s in [NSScreen screens]) {
		NSNumber *num = s.deviceDescription[@"NSScreenNumber"];
		if (num != nil && CGDisplayIsBuiltin((CGDirectDisplayID)num.unsignedIntValue)) {
			return s.visibleFrame;
		}
	}
	return [NSScreen screens].firstObject.visibleFrame;
}

static void
pump_events(void)
{
	@autoreleasepool {
		NSEvent *ev;
		while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny
		                                untilDate:nil
		                                   inMode:NSDefaultRunLoopMode
		                                  dequeue:YES]) != nil) {
			[NSApp sendEvent:ev];
		}
	}
}

//! Fill every image of @p sc with the BGRA pixels produced by @p fill(x, y, out).
template <typename F>
static bool
fill_swapchain(XrSwapchain sc, uint32_t w, uint32_t h, F fill)
{
	uint32_t n = 0;
	xrEnumerateSwapchainImages(sc, 0, &n, nullptr);
	std::vector<XrSwapchainImageMetalKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR});
	if (n == 0 || XR_FAILED(xrEnumerateSwapchainImages(sc, n, &n, (XrSwapchainImageBaseHeader *)imgs.data()))) {
		return false;
	}
	std::vector<uint8_t> px((size_t)w * h * 4);
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			fill(x, y, &px[((size_t)y * w + x) * 4]);
		}
	}
	for (uint32_t i = 0; i < n; i++) {
		XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		uint32_t idx = 0;
		if (XR_FAILED(xrAcquireSwapchainImage(sc, &ai, &idx))) {
			return false;
		}
		XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		wi.timeout = XR_INFINITE_DURATION;
		xrWaitSwapchainImage(sc, &wi);
		id<MTLTexture> tex = (__bridge id<MTLTexture>)imgs[idx].texture;
		[tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px.data() bytesPerRow:w * 4];
		XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		xrReleaseSwapchainImage(sc, &ri);
	}
	return true;
}

static XrSwapchain
make_swapchain(XrSession s, int64_t format, uint32_t w, uint32_t h)
{
	XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	sci.format = format;
	sci.sampleCount = 1;
	sci.width = w;
	sci.height = h;
	sci.faceCount = 1;
	sci.arraySize = 1;
	sci.mipCount = 1;
	XrSwapchain sc = XR_NULL_HANDLE;
	if (XR_FAILED(xrCreateSwapchain(s, &sci, &sc))) {
		LOG("xrCreateSwapchain(format %lld) failed", (long long)format);
		return XR_NULL_HANDLE;
	}
	return sc;
}

int
main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	const bool proj_srgb = !env_is("DXR_PROBE_PROJ", "unorm");
	const bool want_ws = !env_is("DXR_PROBE_WS", "0");
	const bool texture_mode = env_is("DXR_PROBE_TEXTURE", "1");
	const bool transparent = env_is("DXR_PROBE_TRANSPARENT", "1");
	const int frames_before_capture = env_int("DXR_PROBE_FRAMES", 90);
	{
		const char *tmp = getenv("TMPDIR");
		std::string dir = (tmp != nullptr && *tmp != '\0') ? tmp : "/tmp";
		if (dir.back() == '/') {
			dir.pop_back();
		}
		g_trigger = dir + "/displayxr_atlas_trigger." + getprogname();
		g_atlas = dir + "/displayxr_atlas." + getprogname() + ".png";
	}
	unlink(g_atlas.c_str());
	unlink(g_trigger.c_str());

	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

	const uint32_t win_w = 800, win_h = 480;
	NSWindow *window = nil;
	NSView *view = nil;
	IOSurfaceRef surface = NULL;
	if (texture_mode) {
		NSDictionary *props = @{
			(id)kIOSurfaceWidth: @(win_w * 2),
			(id)kIOSurfaceHeight: @(win_h * 2),
			(id)kIOSurfaceBytesPerElement: @4,
			(id)kIOSurfacePixelFormat: @((uint32_t)'BGRA'),
		};
		surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
	} else {
		const NSRect vf = builtin_screen_frame();
		const NSRect frame = NSMakeRect(vf.origin.x + 40, vf.origin.y + 40, win_w, win_h);
		window = [[NSWindow alloc] initWithContentRect:frame
		                                     styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
		                                       backing:NSBackingStoreBuffered
		                                         defer:NO];
		[window setTitle:@"colour_probe_metal_macos"];
		view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, win_w, win_h)];
		[view setWantsLayer:YES];
		[window setContentView:view];
		[window makeKeyAndOrderFront:nil];
		pump_events();
	}

	// Instance: Metal + the Cocoa binding (window-space layers ride it).
	std::vector<const char *> exts = {XR_KHR_METAL_ENABLE_EXTENSION_NAME, XR_DXR_COCOA_WINDOW_BINDING_EXTENSION_NAME};
	XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
	strncpy(ici.applicationInfo.applicationName, "colour_probe_metal_macos", XR_MAX_APPLICATION_NAME_SIZE - 1);
	ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	ici.enabledExtensionCount = (uint32_t)exts.size();
	ici.enabledExtensionNames = exts.data();
	XrInstance instance = XR_NULL_HANDLE;
	XR_OK(xrCreateInstance(&ici, &instance));

	XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	XR_OK(xrGetSystem(instance, &sgi, &system));

	PFN_xrGetMetalGraphicsRequirementsKHR get_reqs = nullptr;
	XR_OK(xrGetInstanceProcAddr(instance, "xrGetMetalGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&get_reqs));
	XrGraphicsRequirementsMetalKHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
	XR_OK(get_reqs(instance, system, &reqs));

	uint32_t vc = 0;
	XR_OK(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &vc,
	                                        nullptr));
	std::vector<XrViewConfigurationView> cfg(vc, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
	XR_OK(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, vc, &vc,
	                                        cfg.data()));

	id<MTLDevice> device = (__bridge id<MTLDevice>)reqs.metalDevice;
	if (device == nil) {
		device = MTLCreateSystemDefaultDevice();
	}
	id<MTLCommandQueue> queue = [device newCommandQueue];

	XrGraphicsBindingMetalKHR mb = {XR_TYPE_GRAPHICS_BINDING_METAL_KHR};
	mb.commandQueue = (__bridge void *)queue;
	XrCocoaWindowBindingCreateInfoDXR cb = {};
	cb.type = XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_DXR;
	cb.viewHandle = (__bridge void *)view;
	cb.sharedIOSurface = (void *)surface;
	cb.transparentBackgroundEnabled = transparent ? XR_TRUE : XR_FALSE;
	mb.next = &cb;
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &mb;
	sci.systemId = system;
	XrSession session = XR_NULL_HANDLE;
	XR_OK(xrCreateSession(instance, &sci, &session));

	XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rsci.poseInReferenceSpace.orientation.w = 1.0f;
	XrSpace space = XR_NULL_HANDLE;
	XR_OK(xrCreateReferenceSpace(session, &rsci, &space));

	const int64_t kBGRA = (int64_t)MTLPixelFormatBGRA8Unorm;
	const int64_t kBGRA_SRGB = (int64_t)MTLPixelFormatBGRA8Unorm_sRGB;

	// Projection: one swapchain, two views side by side.
	const uint32_t vw = cfg[0].recommendedImageRectWidth > 0 ? cfg[0].recommendedImageRectWidth : 640;
	const uint32_t vh = cfg[0].recommendedImageRectHeight > 0 ? cfg[0].recommendedImageRectHeight : 480;
	XrSwapchain proj = make_swapchain(session, proj_srgb ? kBGRA_SRGB : kBGRA, vw * 2, vh);
	if (proj == XR_NULL_HANDLE) {
		return 1;
	}
	fill_swapchain(proj, vw * 2, vh, [&](uint32_t x, uint32_t, uint8_t *o) {
		const bool left = (x % vw) < vw / 2;
		const uint8_t v = left ? 38 : (transparent ? 0 : 200);
		o[0] = o[1] = o[2] = v;
		o[3] = (left || !transparent) ? 255 : 0;
	});

	WsLayer ws[] = {
	    {"L1_srgb_opaque_38", true, {38, 38, 38, 255}, false, 0.05f, 0.05f, 0.15f, 0.35f},
	    {"L2_unorm_opaque_38", false, {38, 38, 38, 255}, false, 0.25f, 0.05f, 0.15f, 0.35f},
	    {"L3_srgb_black_a128_over200", true, {0, 0, 0, 128}, true, 0.60f, 0.05f, 0.30f, 0.35f},
	    {"L4_srgb_white_a128_over38", true, {255, 255, 255, 128}, true, 0.05f, 0.50f, 0.40f, 0.35f},
	};
	const uint32_t ws_px = 64;
	if (want_ws) {
		for (WsLayer &l : ws) {
			l.sc = make_swapchain(session, l.srgb ? kBGRA_SRGB : kBGRA, ws_px, ws_px);
			if (l.sc == XR_NULL_HANDLE) {
				return 1;
			}
			fill_swapchain(l.sc, ws_px, ws_px, [&](uint32_t, uint32_t, uint8_t *o) { memcpy(o, l.bgra, 4); });
		}
	}
	LOG("config: projection=%s window_space=%s mode=%s view=%ux%u", proj_srgb ? "SRGB" : "UNORM",
	    want_ws ? "yes" : "no", texture_mode ? "texture(IOSurface)" : "handle(window)", vw, vh);

	bool running = false;
	bool done = false;
	int frame = 0;
	int waited = 0;
	while (!done) {
		if (!texture_mode) {
			pump_events();
		}
		XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
		while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
			if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
				const XrSessionState st = ((XrEventDataSessionStateChanged *)&ev)->state;
				if (st == XR_SESSION_STATE_READY) {
					XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
					bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					running = XR_SUCCEEDED(xrBeginSession(session, &bi));
				} else if (st == XR_SESSION_STATE_STOPPING || st == XR_SESSION_STATE_EXITING) {
					done = true;
				}
			}
			ev = {XR_TYPE_EVENT_DATA_BUFFER};
		}
		if (!running) {
			usleep(10000);
			if (++waited > 1000) {
				LOG("session never became READY");
				return 1;
			}
			continue;
		}

		XrFrameState fs = {XR_TYPE_FRAME_STATE};
		XrFrameWaitInfo fwi = {XR_TYPE_FRAME_WAIT_INFO};
		if (XR_FAILED(xrWaitFrame(session, &fwi, &fs))) {
			continue;
		}
		XrFrameBeginInfo fbi = {XR_TYPE_FRAME_BEGIN_INFO};
		xrBeginFrame(session, &fbi);

		XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
		XrViewState vs = {XR_TYPE_VIEW_STATE};
		XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
		vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		vli.displayTime = fs.predictedDisplayTime;
		vli.space = space;
		uint32_t located = 0;
		xrLocateViews(session, &vli, &vs, 2, &located, views);

		XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
		                                          {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
		for (uint32_t i = 0; i < 2; i++) {
			pv[i].pose = views[i].pose;
			pv[i].fov = views[i].fov;
			pv[i].subImage.swapchain = proj;
			pv[i].subImage.imageRect.offset = {(int32_t)(i * vw), 0};
			pv[i].subImage.imageRect.extent = {(int32_t)vw, (int32_t)vh};
		}
		XrCompositionLayerProjection pl = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		pl.space = space;
		pl.viewCount = 2;
		pl.views = pv;

		std::vector<const XrCompositionLayerBaseHeader *> layers = {(XrCompositionLayerBaseHeader *)&pl};
		XrCompositionLayerWindowSpaceDXR wl[4] = {};
		if (want_ws) {
			for (uint32_t i = 0; i < 4; i++) {
				wl[i].type = XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR;
				wl[i].layerFlags = ws[i].straight ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
				wl[i].subImage.swapchain = ws[i].sc;
				wl[i].subImage.imageRect.extent = {(int32_t)ws_px, (int32_t)ws_px};
				wl[i].x = ws[i].x;
				wl[i].y = ws[i].y;
				wl[i].width = ws[i].w;
				wl[i].height = ws[i].h;
				wl[i].disparity = 0.0f;
				layers.push_back((XrCompositionLayerBaseHeader *)&wl[i]);
			}
		}

		XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
		fei.displayTime = fs.predictedDisplayTime;
		fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		fei.layerCount = fs.shouldRender ? (uint32_t)layers.size() : 0;
		fei.layers = layers.data();
		xrEndFrame(session, &fei);

		frame++;
		if (frame == frames_before_capture) {
			FILE *t = fopen(g_trigger.c_str(), "w");
			if (t != nullptr) {
				fclose(t);
			}
		}
		struct stat st;
		if (frame > frames_before_capture && stat(g_atlas.c_str(), &st) == 0 && st.st_size > 0 &&
		    stat(g_trigger.c_str(), &st) != 0) {
			usleep(200000); // the PNG is written synchronously in layer_commit; settle anyway
			LOG("atlas written: %s", g_atlas.c_str());
			done = true;
		}
		if (frame > frames_before_capture + 300) {
			LOG("no atlas after 300 frames — trigger not consumed");
			done = true;
		}
	}

	xrDestroySession(session);
	xrDestroyInstance(instance);
	if (surface != NULL) {
		CFRelease(surface);
	}
	return 0;
}
