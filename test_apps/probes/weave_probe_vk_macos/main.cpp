// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_weave macOS probe (#759) — headless present-owner harness.
 *
 * The macOS sibling of weave_rpc_probe_d3d11_win (#625), reduced to the pure
 * service contract — no window, no presentation, CPU-verifiable:
 *
 *   1. Creates a headless Vulkan (MoltenVK) device — the only IPC-capable
 *      graphics binding on macOS — and a forced-IPC OpenXR session.
 *   2. xrWeaveBindWindowDXR(fake id) once.
 *   3. CPU-fills a window-sized BGRA IOSurface with two squeezed-SBS rects:
 *        rect A: left eye WHITE, right eye BLACK  -> anaglyph weave = RED
 *        rect B: left eye BLACK, right eye WHITE  -> anaglyph weave = CYAN
 *      (sim_display anaglyph.frag: out = (left.r, right.g, right.b, ...).)
 *   4. Batched xrWeaveSubmitDXR (XrWeaveSubmitRectsDXR, spec v3): input is the
 *      window-sized surface, content at each rect's own window position.
 *   5. First submit hands back the weaved output IOSurfaceRef; completion is
 *      SYNCHRONOUS on macOS (no fence handle — submit returning is the signal).
 *   6. Reads the output back on the CPU, asserts the two rect centres weave to
 *      red / cyan, dumps /tmp/weave_probe_macos_output.ppm, exits 0/1.
 *
 * Run (service already started with the sim display plug-in):
 *   SIM_DISPLAY_OUTPUT=anaglyph XRT_PLUGIN_SEARCH_PATH=.../plugins displayxr-service &
 *   XRT_FORCE_MODE=ipc XR_RUNTIME_JSON=build/openxr_displayxr-dev.json \
 *     ./weave_probe_gl_macos
 */

#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>

#include <vulkan/vulkan.h>

#define XR_USE_PLATFORM_MACOS 1
#define XR_USE_GRAPHICS_API_VULKAN 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_DXR_weave.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define LOG(...)                                                                                                       \
	do {                                                                                                           \
		fprintf(stderr, "[weave_probe] " __VA_ARGS__);                                                         \
		fprintf(stderr, "\n");                                                                                 \
	} while (0)

#define XR_CHECK(call)                                                                                                 \
	do {                                                                                                           \
		XrResult _r = (call);                                                                                  \
		if (XR_FAILED(_r)) {                                                                                   \
			LOG("FAILED %s -> %d", #call, (int)_r);                                                        \
			return 1;                                                                                      \
		}                                                                                                      \
	} while (0)

#define VK_CHECK(call)                                                                                                 \
	do {                                                                                                           \
		VkResult _v = (call);                                                                                  \
		if (_v != VK_SUCCESS) {                                                                                \
			LOG("FAILED %s -> %d", #call, (int)_v);                                                        \
			return 1;                                                                                      \
		}                                                                                                      \
	} while (0)

// Window-client-sized input (the v3 batch contract) + the two weaved rects.
static const uint32_t kWinW = 1280;
static const uint32_t kWinH = 720;
static const XrRect2Di kRectA = {{100, 100}, {320, 180}};
static const XrRect2Di kRectB = {{700, 400}, {400, 200}};
// v4 overlay atlas: an opaque magenta bar near the top, in a gap that overlaps
// neither weaved rect (so the base rects still verify red/cyan) and a background
// gap (so firstChunk transparency is verifiable outside it).
static const XrRect2Di kOverlayBar = {{200, 30}, {800, 40}};

//! BGRA pixel write helper.
static inline void
put_px(uint8_t *base, size_t stride, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
	uint8_t *p = base + (size_t)y * stride + (size_t)x * 4;
	p[0] = b;
	p[1] = g;
	p[2] = r;
	p[3] = 0xFF;
}

/*!
 * Fill one squeezed-SBS rect: the rect's LEFT half holds the left-eye view,
 * the RIGHT half the right-eye view — each squeezed to half the rect width.
 */
static void
fill_sbs_rect(uint8_t *base, size_t stride, const XrRect2Di &r, uint8_t left_lum, uint8_t right_lum)
{
	int half = r.extent.width / 2;
	for (int y = r.offset.y; y < r.offset.y + r.extent.height; y++) {
		for (int x = r.offset.x; x < r.offset.x + r.extent.width; x++) {
			bool is_left = (x - r.offset.x) < half;
			uint8_t lum = is_left ? left_lum : right_lum;
			put_px(base, stride, x, y, lum, lum, lum);
		}
	}
}

static bool
sample_px(IOSurfaceRef surf, int x, int y, uint8_t *out_r, uint8_t *out_g, uint8_t *out_b)
{
	if (IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) {
		return false;
	}
	const uint8_t *base = (const uint8_t *)IOSurfaceGetBaseAddress(surf);
	size_t stride = IOSurfaceGetBytesPerRow(surf);
	const uint8_t *p = base + (size_t)y * stride + (size_t)x * 4;
	*out_b = p[0];
	*out_g = p[1];
	*out_r = p[2];
	IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
	return true;
}

//! BGRA sample including alpha (for the v5 firstChunk transparency assertion).
static bool
sample_px4(IOSurfaceRef surf, int x, int y, uint8_t *out_r, uint8_t *out_g, uint8_t *out_b, uint8_t *out_a)
{
	if (IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) {
		return false;
	}
	const uint8_t *base = (const uint8_t *)IOSurfaceGetBaseAddress(surf);
	size_t stride = IOSurfaceGetBytesPerRow(surf);
	const uint8_t *p = base + (size_t)y * stride + (size_t)x * 4;
	*out_b = p[0];
	*out_g = p[1];
	*out_r = p[2];
	*out_a = p[3];
	IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
	return true;
}

static void
dump_ppm(IOSurfaceRef surf, const char *path)
{
	if (IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) {
		return;
	}
	uint32_t w = (uint32_t)IOSurfaceGetWidth(surf);
	uint32_t h = (uint32_t)IOSurfaceGetHeight(surf);
	const uint8_t *base = (const uint8_t *)IOSurfaceGetBaseAddress(surf);
	size_t stride = IOSurfaceGetBytesPerRow(surf);
	FILE *f = fopen(path, "wb");
	if (f != NULL) {
		fprintf(f, "P6\n%u %u\n255\n", w, h);
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t *row = base + (size_t)y * stride;
			for (uint32_t x = 0; x < w; x++) {
				uint8_t rgb[3] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0]};
				fwrite(rgb, 1, 3, f);
			}
		}
		fclose(f);
		LOG("dumped weaved output -> %s (%ux%u)", path, w, h);
	}
	IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
}

static IOSurfaceRef
create_input_surface(void)
{
	// kIOSurfaceIsGlobal so the service-side IOSurfaceLookup(id) finds it
	// (the IPC channel transports the bare IOSurfaceID).
	int32_t w = (int32_t)kWinW, h = (int32_t)kWinH, bpe = 4;
	uint32_t fmt = 'BGRA';
	CFStringRef keys[5] = {CFSTR("IOSurfaceWidth"), CFSTR("IOSurfaceHeight"), CFSTR("IOSurfaceBytesPerElement"),
	                       CFSTR("IOSurfacePixelFormat"), CFSTR("IOSurfaceIsGlobal")};
	CFNumberRef vals[4] = {
	    CFNumberCreate(NULL, kCFNumberSInt32Type, &w),
	    CFNumberCreate(NULL, kCFNumberSInt32Type, &h),
	    CFNumberCreate(NULL, kCFNumberSInt32Type, &bpe),
	    CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt),
	};
	CFTypeRef values[5] = {vals[0], vals[1], vals[2], vals[3], kCFBooleanTrue};
	CFDictionaryRef props = CFDictionaryCreate(NULL, (const void **)keys, (const void **)values, 5,
	                                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	IOSurfaceRef surf = IOSurfaceCreate(props);
	CFRelease(props);
	for (int i = 0; i < 4; i++) {
		CFRelease(vals[i]);
	}
	return surf;
}

/*!
 * v4 overlay atlas (browser#18): a window-sized PREMULTIPLIED-alpha BGRA surface,
 * transparent (0,0,0,0) everywhere except one opaque magenta bar. Opaque premul ==
 * straight, so the bar is a plain magenta with alpha 255. Global so the service's
 * IOSurfaceLookup(id) finds it (the IPC transports the bare IOSurfaceID).
 */
static IOSurfaceRef
create_overlay_surface(void)
{
	int32_t w = (int32_t)kWinW, h = (int32_t)kWinH, bpe = 4;
	uint32_t fmt = 'BGRA';
	CFStringRef keys[5] = {CFSTR("IOSurfaceWidth"), CFSTR("IOSurfaceHeight"), CFSTR("IOSurfaceBytesPerElement"),
	                       CFSTR("IOSurfacePixelFormat"), CFSTR("IOSurfaceIsGlobal")};
	CFNumberRef vals[4] = {
	    CFNumberCreate(NULL, kCFNumberSInt32Type, &w),
	    CFNumberCreate(NULL, kCFNumberSInt32Type, &h),
	    CFNumberCreate(NULL, kCFNumberSInt32Type, &bpe),
	    CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt),
	};
	CFTypeRef values[5] = {vals[0], vals[1], vals[2], vals[3], kCFBooleanTrue};
	CFDictionaryRef props = CFDictionaryCreate(NULL, (const void **)keys, (const void **)values, 5,
	                                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	IOSurfaceRef surf = IOSurfaceCreate(props);
	CFRelease(props);
	for (int i = 0; i < 4; i++) {
		CFRelease(vals[i]);
	}
	if (surf == NULL) {
		return NULL;
	}
	IOSurfaceLock(surf, 0, NULL);
	uint8_t *base = (uint8_t *)IOSurfaceGetBaseAddress(surf);
	size_t stride = IOSurfaceGetBytesPerRow(surf);
	// Transparent premultiplied field everywhere (all four channels 0).
	for (uint32_t y = 0; y < kWinH; y++) {
		memset(base + (size_t)y * stride, 0, (size_t)kWinW * 4);
	}
	// Opaque magenta bar (premul == straight when a=255): B=230, G=13, R=230, A=255.
	for (int y = kOverlayBar.offset.y; y < kOverlayBar.offset.y + kOverlayBar.extent.height; y++) {
		for (int x = kOverlayBar.offset.x; x < kOverlayBar.offset.x + kOverlayBar.extent.width; x++) {
			uint8_t *p = base + (size_t)y * stride + (size_t)x * 4;
			p[0] = 230; // B
			p[1] = 13;  // G
			p[2] = 230; // R
			p[3] = 255; // A (opaque)
		}
	}
	IOSurfaceUnlock(surf, 0, NULL);
	return surf;
}

//! Generic global BGRA IOSurface (kIOSurfaceIsGlobal so the service-side
//! IOSurfaceLookup(id) finds it — the IPC transports the bare IOSurfaceID).
static IOSurfaceRef
create_bgra_surface(int32_t w, int32_t h)
{
	int32_t bpe = 4;
	uint32_t fmt = 'BGRA';
	CFStringRef keys[5] = {CFSTR("IOSurfaceWidth"), CFSTR("IOSurfaceHeight"), CFSTR("IOSurfaceBytesPerElement"),
	                       CFSTR("IOSurfacePixelFormat"), CFSTR("IOSurfaceIsGlobal")};
	CFNumberRef vals[4] = {
	    CFNumberCreate(NULL, kCFNumberSInt32Type, &w),
	    CFNumberCreate(NULL, kCFNumberSInt32Type, &h),
	    CFNumberCreate(NULL, kCFNumberSInt32Type, &bpe),
	    CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt),
	};
	CFTypeRef values[5] = {vals[0], vals[1], vals[2], vals[3], kCFBooleanTrue};
	CFDictionaryRef props = CFDictionaryCreate(NULL, (const void **)keys, (const void **)values, 5,
	                                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	IOSurfaceRef surf = IOSurfaceCreate(props);
	CFRelease(props);
	for (int i = 0; i < 4; i++) {
		CFRelease(vals[i]);
	}
	return surf;
}

//! Spec-v6 N-view atlas (#774): build a 2-view (2x1) atlas of size 2*cvw x cvh,
//! tile 0 (left eye) solid RED, tile 1 (right eye) solid CYAN. The anaglyph DP
//! (out = left.r, right.g, right.b) then weaves this to WHITE. Tiles are packed
//! CONTIGUOUSLY at content-view stride cvw, so 2*cvw == atlas width (zero-copy).
static IOSurfaceRef
create_nview_atlas(uint32_t cvw, uint32_t cvh)
{
	IOSurfaceRef surf = create_bgra_surface((int32_t)(2 * cvw), (int32_t)cvh);
	if (surf == NULL) {
		return NULL;
	}
	IOSurfaceLock(surf, 0, NULL);
	uint8_t *base = (uint8_t *)IOSurfaceGetBaseAddress(surf);
	size_t stride = IOSurfaceGetBytesPerRow(surf);
	for (uint32_t y = 0; y < cvh; y++) {
		for (uint32_t x = 0; x < cvw; x++) {
			put_px(base, stride, (int)x, (int)y, 0xFF, 0x00, 0x00);       // tile0 left = RED
			put_px(base, stride, (int)(cvw + x), (int)y, 0x00, 0xFF, 0xFF); // tile1 right = CYAN
		}
	}
	IOSurfaceUnlock(surf, 0, NULL);
	return surf;
}

//! Split a space-separated extension string into stable storage + pointers.
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

int
main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	// ---- Instance with the weave + vulkan-enable extensions.
	uint32_t ext_count = 0;
	XR_CHECK(xrEnumerateInstanceExtensionProperties(NULL, 0, &ext_count, NULL));
	std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
	XR_CHECK(xrEnumerateInstanceExtensionProperties(NULL, ext_count, &ext_count, exts.data()));
	bool has_weave = false, has_vk = false;
	for (const auto &e : exts) {
		if (strcmp(e.extensionName, XR_DXR_WEAVE_EXTENSION_NAME) == 0) {
			has_weave = true;
		}
		if (strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) {
			has_vk = true;
		}
	}
	LOG("XR_DXR_weave:         %s", has_weave ? "AVAILABLE" : "NOT FOUND");
	LOG("XR_KHR_vulkan_enable: %s", has_vk ? "AVAILABLE" : "NOT FOUND");
	if (!has_weave || !has_vk) {
		return 1;
	}

	const char *enabled[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_DXR_WEAVE_EXTENSION_NAME};
	XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
	snprintf(ici.applicationInfo.applicationName, sizeof(ici.applicationInfo.applicationName), "%s",
	         "DXRWeaveProbeMacOS");
	ici.applicationInfo.applicationVersion = 1;
	snprintf(ici.applicationInfo.engineName, sizeof(ici.applicationInfo.engineName), "%s", "None");
	ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	ici.enabledExtensionCount = 2;
	ici.enabledExtensionNames = enabled;
	XrInstance instance = XR_NULL_HANDLE;
	XR_CHECK(xrCreateInstance(&ici, &instance));

	XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId system_id = XR_NULL_SYSTEM_ID;
	XR_CHECK(xrGetSystem(instance, &sgi, &system_id));

	// ---- Headless Vulkan (MoltenVK) device per XR_KHR_vulkan_enable.
	PFN_xrGetVulkanGraphicsRequirementsKHR pfn_req = NULL;
	PFN_xrGetVulkanInstanceExtensionsKHR pfn_iext = NULL;
	PFN_xrGetVulkanDeviceExtensionsKHR pfn_dext = NULL;
	PFN_xrGetVulkanGraphicsDeviceKHR pfn_gdev = NULL;
	xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&pfn_req);
	xrGetInstanceProcAddr(instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction *)&pfn_iext);
	xrGetInstanceProcAddr(instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction *)&pfn_dext);
	xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction *)&pfn_gdev);
	if (pfn_req == NULL || pfn_iext == NULL || pfn_dext == NULL || pfn_gdev == NULL) {
		LOG("failed to resolve XR_KHR_vulkan_enable entry points");
		return 1;
	}
	XrGraphicsRequirementsVulkanKHR vk_req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
	XR_CHECK(pfn_req(instance, system_id, &vk_req));

	uint32_t len = 0;
	pfn_iext(instance, system_id, 0, &len, NULL);
	std::string iext_str(len, '\0');
	pfn_iext(instance, system_id, len, &len, iext_str.data());
	std::vector<std::string> iext_storage;
	std::vector<const char *> iexts;
	split_exts(iext_str, iext_storage, iexts);

	// MoltenVK is a portability implementation — enumerate it.
	uint32_t avail = 0;
	vkEnumerateInstanceExtensionProperties(NULL, &avail, NULL);
	std::vector<VkExtensionProperties> avail_exts(avail);
	vkEnumerateInstanceExtensionProperties(NULL, &avail, avail_exts.data());
	bool has_portability = false;
	for (const auto &e : avail_exts) {
		if (strcmp(e.extensionName, "VK_KHR_portability_enumeration") == 0) {
			has_portability = true;
			iexts.push_back("VK_KHR_portability_enumeration");
			break;
		}
	}

	VkApplicationInfo app_info = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app_info.pApplicationName = "DXRWeaveProbeMacOS";
	app_info.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo vici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	vici.pApplicationInfo = &app_info;
	vici.enabledExtensionCount = (uint32_t)iexts.size();
	vici.ppEnabledExtensionNames = iexts.data();
	if (has_portability) {
		vici.flags |= 0x00000001; // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
	}
	VkInstance vk_instance = VK_NULL_HANDLE;
	VK_CHECK(vkCreateInstance(&vici, NULL, &vk_instance));

	VkPhysicalDevice phys = VK_NULL_HANDLE;
	XR_CHECK(pfn_gdev(instance, system_id, vk_instance, &phys));

	uint32_t qf_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, NULL);
	std::vector<VkQueueFamilyProperties> qfs(qf_count);
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, qfs.data());
	uint32_t qfi = UINT32_MAX;
	for (uint32_t i = 0; i < qf_count; i++) {
		if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			qfi = i;
			break;
		}
	}
	if (qfi == UINT32_MAX) {
		LOG("no graphics queue family");
		return 1;
	}

	len = 0;
	pfn_dext(instance, system_id, 0, &len, NULL);
	std::string dext_str(len, '\0');
	pfn_dext(instance, system_id, len, &len, dext_str.data());
	std::vector<std::string> dext_storage;
	std::vector<const char *> dexts;
	split_exts(dext_str, dext_storage, dexts);

	// MoltenVK devices require VK_KHR_portability_subset when advertised.
	uint32_t dav = 0;
	vkEnumerateDeviceExtensionProperties(phys, NULL, &dav, NULL);
	std::vector<VkExtensionProperties> dav_exts(dav);
	vkEnumerateDeviceExtensionProperties(phys, NULL, &dav, dav_exts.data());
	for (const auto &e : dav_exts) {
		if (strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) {
			bool already = false;
			for (const char *d : dexts) {
				if (strcmp(d, "VK_KHR_portability_subset") == 0) {
					already = true;
				}
			}
			if (!already) {
				dexts.push_back("VK_KHR_portability_subset");
			}
			break;
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
	VkDevice device = VK_NULL_HANDLE;
	VK_CHECK(vkCreateDevice(phys, &dci, NULL, &device));

	// ---- Forced-IPC session over the headless Vulkan device.
	XrGraphicsBindingVulkanKHR vk_binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
	vk_binding.instance = vk_instance;
	vk_binding.physicalDevice = phys;
	vk_binding.device = device;
	vk_binding.queueFamilyIndex = qfi;
	vk_binding.queueIndex = 0;
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &vk_binding;
	sci.systemId = system_id;
	XrSession session = XR_NULL_HANDLE;
	XR_CHECK(xrCreateSession(instance, &sci, &session));
	LOG("session created (IPC, headless Vulkan)");

	PFN_xrWeaveBindWindowDXR pfn_bind = NULL;
	PFN_xrWeaveSubmitDXR pfn_submit = NULL;
	xrGetInstanceProcAddr(instance, "xrWeaveBindWindowDXR", (PFN_xrVoidFunction *)&pfn_bind);
	xrGetInstanceProcAddr(instance, "xrWeaveSubmitDXR", (PFN_xrVoidFunction *)&pfn_submit);
	if (pfn_bind == NULL || pfn_submit == NULL) {
		LOG("failed to resolve weave entry points");
		return 1;
	}

	XR_CHECK(pfn_bind(session, (void *)(uintptr_t)0x1)); // fake window id (phase-anchor only)

	// ---- Build the window-sized squeezed-SBS input.
	IOSurfaceRef input = create_input_surface();
	if (input == NULL) {
		LOG("IOSurfaceCreate failed");
		return 1;
	}
	IOSurfaceLock(input, 0, NULL);
	uint8_t *base = (uint8_t *)IOSurfaceGetBaseAddress(input);
	size_t stride = IOSurfaceGetBytesPerRow(input);
	for (uint32_t y = 0; y < kWinH; y++) { // dark-gray background
		for (uint32_t x = 0; x < kWinW; x++) {
			put_px(base, stride, (int)x, (int)y, 32, 32, 32);
		}
	}
	fill_sbs_rect(base, stride, kRectA, /*left*/ 0xFF, /*right*/ 0x00); // -> RED after anaglyph
	fill_sbs_rect(base, stride, kRectB, /*left*/ 0x00, /*right*/ 0xFF); // -> CYAN after anaglyph
	IOSurfaceUnlock(input, 0, NULL);

	// ---- v4 overlay atlas (premul-RGBA magenta bar), chained on the submit.
	IOSurfaceRef overlay = create_overlay_surface();
	if (overlay == NULL) {
		LOG("overlay IOSurfaceCreate failed");
		return 1;
	}

	// ---- Batched submits (spec v3) + v5 firstChunk + v4 overlay (spec v4).
	XrRect2Di rects[2] = {kRectA, kRectB};
	XrWeaveSubmitOverlaysDXR ov = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_OVERLAYS_DXR};
	ov.overlayTexture = (void *)overlay;
	ov.overlayIsDxgi = XR_FALSE;
	ov.rectCount = 0; // whole-atlas composite (premul alpha authoritative)
	ov.rects = NULL;
	XrWeaveSubmitRectsDXR batch = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_RECTS_DXR};
	batch.next = &ov; // chain overlays after rects
	batch.rectCount = 2;
	batch.rects = rects;
	XrWeaveSubmitInfoDXR submit = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_INFO_DXR};
	submit.next = &batch;
	submit.inputTexture = (void *)input;
	submit.inputIsDxgi = XR_FALSE;
	submit.firstChunk = XR_TRUE; // single submit per frame → first → clears woven output transparent (v5)

	IOSurfaceRef output = NULL;
	uint32_t out_w = 0, out_h = 0;
	for (int frame = 0; frame < 5; frame++) {
		XrWeaveOutputDXR out = {(XrStructureType)XR_TYPE_WEAVE_OUTPUT_DXR};
		XrResult r = pfn_submit(session, &submit, &out);
		if (XR_FAILED(r)) {
			LOG("xrWeaveSubmitDXR frame %d FAILED: %d", frame, (int)r);
			return 1;
		}
		if (out.weavedTexture != NULL && output == NULL) {
			output = (IOSurfaceRef)out.weavedTexture; // retained handback
		}
		out_w = out.width;
		out_h = out.height;
		if (frame == 0) {
			LOG("frame 0: weaved %ux%u, fence=%p fenceValue=%llu, eyes=%u (valid=%d tracking=%d)",
			    out.width, out.height, out.fence, (unsigned long long)out.fenceValue, out.eyeCount,
			    (int)out.eyesValid, (int)out.eyesTracking);
		}
	}
	if (output == NULL) {
		LOG("FAIL: no weaved output IOSurface was handed back");
		return 1;
	}
	if (out_w != kWinW || out_h != kWinH) {
		LOG("FAIL: output dims %ux%u != input (window) dims %ux%u", out_w, out_h, kWinW, kWinH);
		return 1;
	}

	// ---- Verify the anaglyph weave on the CPU.
	dump_ppm(output, "/tmp/weave_probe_macos_output.ppm");

	struct Check
	{
		const char *name;
		int x, y;
		bool want_red; // else cyan
	} checks[] = {
	    {"rectA(red)", kRectA.offset.x + kRectA.extent.width / 2, kRectA.offset.y + kRectA.extent.height / 2, true},
	    {"rectB(cyan)", kRectB.offset.x + kRectB.extent.width / 2, kRectB.offset.y + kRectB.extent.height / 2,
	     false},
	};
	bool pass = true;
	for (const auto &c : checks) {
		uint8_t r = 0, g = 0, b = 0;
		if (!sample_px(output, c.x, c.y, &r, &g, &b)) {
			LOG("FAIL: could not sample %s", c.name);
			pass = false;
			continue;
		}
		bool ok = c.want_red ? (r > 150 && g < 80 && b < 80) : (r < 80 && g > 150 && b > 150);
		LOG("%s @(%d,%d) = (%u,%u,%u) -> %s", c.name, c.x, c.y, r, g, b, ok ? "OK" : "WRONG");
		pass = pass && ok;
	}

	// ---- v4 overlay: the magenta bar must be composited over the woven output.
	{
		int bx = kOverlayBar.offset.x + kOverlayBar.extent.width / 2;
		int by = kOverlayBar.offset.y + kOverlayBar.extent.height / 2;
		uint8_t r = 0, g = 0, b = 0, a = 0;
		if (sample_px4(output, bx, by, &r, &g, &b, &a)) {
			bool ok = (r > 150 && b > 150 && g < 80 && a > 200);
			LOG("v4 overlay bar @(%d,%d) = (%u,%u,%u,a=%u) want magenta+opaque -> %s", bx, by, r, g, b, a,
			    ok ? "OK" : "WRONG");
			pass = pass && ok;
		} else {
			LOG("FAIL: could not sample overlay bar");
			pass = false;
		}
	}

	// ---- v5 firstChunk: a background gap (no rect, no overlay) must be
	// transparent (alpha ~0); a woven tile centre must be opaque (alpha ~255).
	{
		uint8_t r = 0, g = 0, b = 0, a = 0;
		if (sample_px4(output, 10, 10, &r, &g, &b, &a)) {
			bool ok = (a < 40);
			LOG("v5 firstChunk gap @(10,10) alpha=%u want ~0 -> %s", a, ok ? "OK" : "WRONG");
			pass = pass && ok;
		} else {
			LOG("FAIL: could not sample firstChunk gap");
			pass = false;
		}
		int tx = kRectA.offset.x + kRectA.extent.width / 2;
		int ty = kRectA.offset.y + kRectA.extent.height / 2;
		if (sample_px4(output, tx, ty, &r, &g, &b, &a)) {
			bool ok = (a > 200);
			LOG("v5 firstChunk tile @(%d,%d) alpha=%u want ~255 -> %s", tx, ty, a, ok ? "OK" : "WRONG");
			pass = pass && ok;
		}
	}

	// ---- Spec-v6 N-view atlas (#774): additive path. A submit that chains
	// XrWeaveSubmitLayoutDXR carries a worst-case-sized packed atlas (tiles
	// contiguous top-left at content-view size), NOT per-rect squeezed SBS. The
	// runtime crops the packed region (here it exactly fills the input →
	// zero-copy) and weaves once; the output is ONE content view (cvw x cvh).
	{
		const uint32_t cvw = 640, cvh = 360;
		IOSurfaceRef nv_input = create_nview_atlas(cvw, cvh); // 2*cvw x cvh, red|cyan
		if (nv_input == NULL) {
			LOG("v6 FAIL: N-view atlas IOSurfaceCreate failed");
			pass = false;
		} else {
			XrWeaveSubmitLayoutDXR lay = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_LAYOUT_DXR};
			lay.viewCount = 2;
			lay.tileColumns = 2;
			lay.tileRows = 1;
			lay.contentViewWidth = cvw;
			lay.contentViewHeight = cvh;
			XrWeaveSubmitInfoDXR v6submit = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_INFO_DXR};
			v6submit.next = &lay; // ONLY the layout chained — no rects, no overlay
			v6submit.inputTexture = (void *)nv_input;
			v6submit.inputIsDxgi = XR_FALSE;
			v6submit.firstChunk = XR_TRUE;

			IOSurfaceRef v6out = NULL;
			uint32_t v6w = 0, v6h = 0;
			bool v6ok = true;
			for (int frame = 0; frame < 3; frame++) {
				XrWeaveOutputDXR out = {(XrStructureType)XR_TYPE_WEAVE_OUTPUT_DXR};
				XrResult r = pfn_submit(session, &v6submit, &out);
				if (XR_FAILED(r)) {
					LOG("v6 FAIL: xrWeaveSubmitDXR frame %d -> %d", frame, (int)r);
					v6ok = false;
					break;
				}
				if (out.weavedTexture != NULL && v6out == NULL) {
					v6out = (IOSurfaceRef)out.weavedTexture;
				}
				v6w = out.width;
				v6h = out.height;
			}
			if (v6ok && v6out == NULL) {
				LOG("v6 FAIL: no weaved output handed back");
				v6ok = false;
			}
			if (v6ok && (v6w != cvw || v6h != cvh)) {
				LOG("v6 FAIL: output dims %ux%u != content view %ux%u", v6w, v6h, cvw, cvh);
				v6ok = false;
			}
			if (v6ok) {
				dump_ppm(v6out, "/tmp/weave_probe_macos_v6_output.ppm");
				// anaglyph(red-left | cyan-right) = (left.r, right.g, right.b) = WHITE.
				uint8_t r = 0, g = 0, b = 0;
				if (sample_px(v6out, (int)cvw / 2, (int)cvh / 2, &r, &g, &b)) {
					bool white = (r > 150 && g > 150 && b > 150);
					bool nonblack = (r + g + b) > 90;
					LOG("v6 centre @(%u,%u) = (%u,%u,%u) want white/non-black -> %s", cvw / 2,
					    cvh / 2, r, g, b, white ? "OK(white)" : (nonblack ? "OK(non-black)" : "WRONG"));
					v6ok = v6ok && nonblack;
				} else {
					LOG("v6 FAIL: could not sample output centre");
					v6ok = false;
				}
			}
			LOG("v6 N-view atlas: %s", v6ok ? "PASS" : "FAIL");
			pass = pass && v6ok;
			if (v6out != NULL) {
				CFRelease(v6out);
			}
			CFRelease(nv_input);
		}
	}

	xrDestroySession(session);
	xrDestroyInstance(instance);
	CFRelease(input);
	CFRelease(overlay);
	CFRelease(output);
	vkDestroyDevice(device, NULL);
	vkDestroyInstance(vk_instance, NULL);

	LOG("%s", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
