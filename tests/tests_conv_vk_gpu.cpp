// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 P5 — the conversion core's Vulkan GPU glue (vk_conv_gpu).
 *
 *  - caps: CPU always; which GPU memory kinds this device can export (logged);
 *  - the allocation rule: providers stay on the CPU lane unless the provider
 *    mask allows a kind; RENDER_TARGET / MIRROR images are always GPU;
 *  - CPU <-> image round trips (RGBA8, R32F, R16F, array layers), the #2030
 *    mirror upload;
 *  - exportable memory: OPAQUE_FD export re-imported into a second VkImage
 *    (Linux), IOSurface export read on the CPU and an IOSurface IMPORT
 *    through VK_EXT_metal_objects (macOS) — the transports the docs call real;
 *  - S6 readback cadence (fresh samples of an earlier copy, never a stall);
 *  - S5 with CPU colour + CPU depth into a GPU target, and into a CPU target;
 *  - a composed VIEWS_MONO conversion through conv_core: a CPU depth provider,
 *    the glue as allocator + S5 + S6, a GPU views result;
 *  - step C (lifted weave rects): the CPU route serves spec-3 SBS streams for
 *    weave rects only, the bridge resolves their tagged ids, the per-DP eyes
 *    sink publishes / retires; and vk_conv_weave on a lifted rect -- flat until
 *    the first result, a snapshot capped to the 1920 long edge, the result
 *    woven into the rect, nothing written outside it, the letterbox crop.
 *
 * SKIPs when there is no Vulkan device.
 *
 * @ingroup tests
 */

#include "catch_amalgamated.hpp"

#include "vktest_conv_bundle.hpp"

#include "conv_fake_provider.h"

#include "conversion/conv_core.h"
#include "server/ipc_server_conv.h"
#include "server/ipc_server_conv_cpu.h"
#include "shared/ipc_cpu_buffer.h"
#include "util/u_handles.h"
#include "vk_conv/vk_conv_gpu.h"
#include "vk_conv/vk_conv_weave.h"

#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_dp_lift.h"

#include "os/os_time.h"

#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_METAL)
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#endif
#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_FD)
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

using namespace vktest_conv;

namespace {

struct Glue
{
	unique_bundle vk;
	vk_conv_gpu *g = nullptr;
	conv_image_alloc_ops ops{};
	conv_viewsyn_ops vs{};
	conv_depth_readback_ops rb{};

	~Glue()
	{
		vk_conv_gpu_destroy(&g);
	}
};

bool
make_glue(Glue &x)
{
	x.vk = make_bundle();
	if (!x.vk) {
		return false;
	}
	REQUIRE(vk_conv_gpu_create(x.vk.get(), &x.g));
	vk_conv_gpu_alloc_ops(x.g, &x.ops);
	vk_conv_gpu_stage_ops(x.g, &x.vs, &x.rb);
	return true;
}

conv_image_request
req_of(uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t layers = 1, uint32_t mem = 0, uint64_t sid = 7)
{
	conv_image_request r{};
	r.width = w;
	r.height = h;
	r.format = fmt;
	r.array_layers = layers;
	r.usage = usage;
	r.stream_id = sid;
	r.import_memory_kinds = mem;
	r.role = fmt == XRT_CONV_FMT_R32_FLOAT || fmt == XRT_CONV_FMT_R16_FLOAT ? XRT_CONV_OUT_DEPTH : 0;
	return r;
}

uint64_t
now_ns()
{
	return os_monotonic_get_ns();
}

bool
wait_until(const std::function<bool()> &pred, uint32_t timeout_ms)
{
	const uint64_t end = now_ns() + (uint64_t)timeout_ms * 1000000ull;
	while (now_ns() < end) {
		if (pred()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return pred();
}

const char *
kind_name(uint32_t k)
{
	switch (k) {
	case XRT_CONV_MEM_CPU: return "CPU";
	case XRT_CONV_MEM_VK_OPAQUE_FD: return "VK_OPAQUE_FD";
	case XRT_CONV_MEM_DMABUF: return "DMABUF";
	case XRT_CONV_MEM_IOSURFACE: return "IOSURFACE";
	default: return "?";
	}
}

} // namespace

TEST_CASE("vk glue: caps name CPU always and the device's exportable kinds", "[conv_vk_gpu]")
{
	Glue x;
	if (!make_glue(x)) {
		SKIP("no Vulkan device available");
	}
	vk_conv_gpu_caps c{};
	vk_conv_gpu_get_caps(x.g, &c);
	CHECK((c.memory_kinds & XRT_CONV_MEM_CPU) != 0u);
	CHECK((c.sync_kinds & (1u << XRT_CONV_SYNC_NONE)) != 0u);
	std::string kinds;
	for (uint32_t k : {XRT_CONV_MEM_CPU, XRT_CONV_MEM_VK_OPAQUE_FD, XRT_CONV_MEM_DMABUF, XRT_CONV_MEM_IOSURFACE}) {
		if ((c.memory_kinds & k) != 0) {
			kinds += std::string(kind_name(k)) + " ";
		}
	}
	SUCCEED("memory kinds: " << kinds << " sync kinds 0x" << std::hex << c.sync_kinds << " timeline "
	                         << c.timeline_semaphore);
#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_METAL)
	// MoltenVK: no external_memory_fd, IOSurface through VK_EXT_metal_objects.
	CHECK((c.memory_kinds & XRT_CONV_MEM_VK_OPAQUE_FD) == 0u);
	CHECK((c.memory_kinds & XRT_CONV_MEM_IOSURFACE) != 0u);
#endif
}

TEST_CASE("vk glue: providers stay on the CPU lane; runtime-only images are GPU", "[conv_vk_gpu]")
{
	Glue x;
	if (!make_glue(x)) {
		SKIP("no Vulkan device available");
	}
	vk_conv_gpu_caps c{};
	vk_conv_gpu_get_caps(x.g, &c);
	const uint32_t gpu_kinds = c.memory_kinds & ~XRT_CONV_MEM_CPU;
	xrt_conv_image img{};

	// Unknown import kinds: declined -> the CPU allocator.
	auto r = req_of(64, 32, XRT_CONV_FMT_RGBA8_UNORM, CONV_IMAGE_USAGE_INPUT);
	CHECK_FALSE(x.ops.alloc(x.ops.ud, &r, &img));
	// A provider that imports GPU kinds, default provider mask 0: declined.
	r = req_of(64, 32, XRT_CONV_FMT_R32_FLOAT, CONV_IMAGE_USAGE_PROVIDER_OUT, 1, XRT_CONV_MEM_CPU | gpu_kinds);
	CHECK_FALSE(x.ops.alloc(x.ops.ud, &r, &img));
	// NV12 never (no conversion path): declined even for a render target.
	r = req_of(64, 32, XRT_CONV_FMT_NV12, CONV_IMAGE_USAGE_RENDER_TARGET);
	CHECK_FALSE(x.ops.alloc(x.ops.ud, &r, &img));

	// The composed views: always a GPU image (exportable or private).
	r = req_of(128, 32, XRT_CONV_FMT_RGBA8_UNORM, CONV_IMAGE_USAGE_RENDER_TARGET | CONV_IMAGE_USAGE_EXPORT);
	REQUIRE(x.ops.alloc(x.ops.ud, &r, &img));
	CHECK(img.memory_kind != XRT_CONV_MEM_CPU);
	CHECK((img.import_key >> 63) == 0u);
	CHECK(img.sync.kind == XRT_CONV_SYNC_NONE);
	CHECK(img.color_space == XRT_CONV_CS_SRGB);
	if (gpu_kinds != 0) {
		CHECK(img.handle != VK_CONV_GPU_NO_HANDLE); // RGBA8 1 layer: exportable everywhere it can be
	}
	CHECK(vk_conv_gpu_image_count(x.g) == 1u);
	x.ops.free(x.ops.ud, &img);
	CHECK(vk_conv_gpu_image_count(x.g) == 0u);

	// ARRAY views (4 layers): GPU too; IOSurface is one layer, so private on macOS.
	r = req_of(64, 32, XRT_CONV_FMT_RGBA8_UNORM, CONV_IMAGE_USAGE_RENDER_TARGET, 4);
	REQUIRE(x.ops.alloc(x.ops.ud, &r, &img));
	CHECK(img.memory_kind != XRT_CONV_MEM_CPU);
	SUCCEED("4-layer render target: kind " << kind_name(img.memory_kind) << ", handle "
	                                       << (img.handle == VK_CONV_GPU_NO_HANDLE ? "private" : "exportable"));
	x.ops.free(x.ops.ud, &img);

	// With the provider mask raised, a provider importing a GPU kind gets one.
	if (gpu_kinds != 0) {
		vk_conv_gpu_set_provider_kinds(x.g, gpu_kinds);
		r = req_of(64, 32, XRT_CONV_FMT_R32_FLOAT, CONV_IMAGE_USAGE_PROVIDER_OUT, 1,
		           XRT_CONV_MEM_CPU | gpu_kinds);
		REQUIRE(x.ops.alloc(x.ops.ud, &r, &img));
		CHECK((img.memory_kind & gpu_kinds) != 0u);
		CHECK(img.handle != VK_CONV_GPU_NO_HANDLE);
		CHECK(img.color_space == XRT_CONV_CS_DATA);
		x.ops.free(x.ops.ud, &img);
		// A provider whose sync kinds exclude NONE: declined (every glue image is SYNC_NONE).
		r.import_sync_kinds = 1u << XRT_CONV_SYNC_VK_SEMAPHORE_FD;
		CHECK_FALSE(x.ops.alloc(x.ops.ud, &r, &img));
	}
}

TEST_CASE("vk glue: CPU <-> image round trips and the mirror upload", "[conv_vk_gpu]")
{
	Glue x;
	if (!make_glue(x)) {
		SKIP("no Vulkan device available");
	}
	struct Case
	{
		uint32_t fmt, bpp, layers;
	} cases[] = {
	    {XRT_CONV_FMT_RGBA8_UNORM, 4, 1}, {XRT_CONV_FMT_RGBA8_SRGB, 4, 1}, {XRT_CONV_FMT_BGRA8_UNORM, 4, 1},
	    {XRT_CONV_FMT_R32_FLOAT, 4, 1},   {XRT_CONV_FMT_R16_FLOAT, 2, 1},  {XRT_CONV_FMT_RGBA8_UNORM, 4, 3},
	};
	for (const Case &k : cases) {
		INFO("fmt " << k.fmt << " layers " << k.layers);
		const uint32_t w = 37, h = 19; // odd sizes: no row-pitch luck
		auto r = req_of(w, h, k.fmt, CONV_IMAGE_USAGE_MIRROR | CONV_IMAGE_USAGE_EXPORT, k.layers);
		xrt_conv_image img{};
		REQUIRE(x.ops.alloc(x.ops.ud, &r, &img));
		std::vector<uint8_t> src((size_t)w * h * k.bpp * k.layers), back(src.size());
		for (size_t i = 0; i < src.size(); i++) {
			src[i] = (uint8_t)(i * 7 + 3);
		}
		REQUIRE(vk_conv_gpu_write(x.g, &img, src.data(), 0));
		REQUIRE(vk_conv_gpu_read(x.g, &img, back.data(), 0));
		CHECK(std::memcmp(src.data(), back.data(), src.size()) == 0);

		// #2030 mirror: a CPU result (strided rows) uploaded through the ops.
		const uint32_t pitch = w * k.bpp + 12;
		std::vector<uint8_t> cpu((size_t)pitch * h * k.layers, 0);
		for (uint32_t l = 0; l < k.layers; l++) {
			for (uint32_t y = 0; y < h; y++) {
				for (uint32_t b = 0; b < w * k.bpp; b++) {
					cpu[((size_t)l * h + y) * pitch + b] = (uint8_t)(b + y * 3 + l * 50);
				}
			}
		}
		xrt_conv_image c{};
		c.memory_kind = XRT_CONV_MEM_CPU;
		c.cpu_ptr = cpu.data();
		c.byte_size = cpu.size();
		c.width = w;
		c.height = h;
		c.format = k.fmt;
		c.row_stride = pitch;
		c.import_key = (1ull << 63) | 5;
		REQUIRE(x.ops.upload(x.ops.ud, &c, &img));
		REQUIRE(vk_conv_gpu_read(x.g, &img, back.data(), 0));
		bool same = true;
		for (uint32_t l = 0; l < k.layers && same; l++) {
			for (uint32_t y = 0; y < h && same; y++) {
				same = std::memcmp(&cpu[((size_t)l * h + y) * pitch],
				                   &back[((size_t)l * h + y) * w * k.bpp], w * k.bpp) == 0;
			}
		}
		CHECK(same);
		// Wrong extent: refused.
		c.width = w - 1;
		CHECK_FALSE(x.ops.upload(x.ops.ud, &c, &img));
		x.ops.free(x.ops.ud, &img);
	}
	CHECK(vk_conv_gpu_image_count(x.g) == 0u);
}

#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_METAL)
TEST_CASE("vk glue (macOS): an exported IOSurface holds the image's bytes", "[conv_vk_gpu][iosurface]")
{
	Glue x;
	if (!make_glue(x)) {
		SKIP("no Vulkan device available");
	}
	vk_conv_gpu_caps c{};
	vk_conv_gpu_get_caps(x.g, &c);
	REQUIRE((c.memory_kinds & XRT_CONV_MEM_IOSURFACE) != 0u);
	const uint32_t w = 64, h = 16;
	auto r = req_of(w, h, XRT_CONV_FMT_BGRA8_UNORM, CONV_IMAGE_USAGE_RENDER_TARGET | CONV_IMAGE_USAGE_EXPORT);
	xrt_conv_image img{};
	REQUIRE(x.ops.alloc(x.ops.ud, &r, &img));
	REQUIRE(img.memory_kind == XRT_CONV_MEM_IOSURFACE);
	REQUIRE(img.handle != VK_CONV_GPU_NO_HANDLE);
	IOSurfaceRef surf = (IOSurfaceRef)(uintptr_t)img.handle;
	CHECK(IOSurfaceGetWidth(surf) == w);
	CHECK(IOSurfaceGetHeight(surf) == h);
	std::vector<uint8_t> src((size_t)w * h * 4);
	for (size_t i = 0; i < src.size(); i++) {
		src[i] = (uint8_t)(i * 5 + 1);
	}
	REQUIRE(vk_conv_gpu_write(x.g, &img, src.data(), 0));
	REQUIRE(IOSurfaceLock(surf, kIOSurfaceLockReadOnly, nullptr) == kIOReturnSuccess);
	const uint8_t *base = (const uint8_t *)IOSurfaceGetBaseAddress(surf);
	const size_t bpr = IOSurfaceGetBytesPerRow(surf);
	bool same = true;
	for (uint32_t y = 0; y < h && same; y++) {
		same = std::memcmp(base + y * bpr, &src[(size_t)y * w * 4], (size_t)w * 4) == 0;
	}
	IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, nullptr);
	CHECK(same);
	SUCCEED("IOSurface export: " << (same ? "bytes match" : "MISMATCH") << ", bytesPerRow " << bpr << ", fourcc 0x"
	                             << std::hex << IOSurfaceGetPixelFormat(surf));
	x.ops.free(x.ops.ud, &img);
}

TEST_CASE("vk glue (macOS): an IOSurface imports into a VkImage through VK_EXT_metal_objects",
          "[conv_vk_gpu][iosurface]")
{
	// The import half the macOS transport needs (a client's IOSurface in):
	// the comp_multi weave engine's pattern, verified against CPU bytes.
	auto vkp = make_bundle();
	if (!vkp) {
		SKIP("no Vulkan device available");
	}
	struct vk_bundle *vk = vkp.get();
	if (!vk->has_EXT_metal_objects || !vk->has_EXT_external_memory_metal) {
		SKIP("VK_EXT_metal_objects / VK_EXT_external_memory_metal not available");
	}
	const int w = 48, h = 8;
	CFMutableDictionaryRef props =
	    CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	int iw = w, ih = h, bpe = 4;
	uint32_t fourcc = 'BGRA';
	CFNumberRef nw = CFNumberCreate(nullptr, kCFNumberIntType, &iw);
	CFNumberRef nh = CFNumberCreate(nullptr, kCFNumberIntType, &ih);
	CFNumberRef nb = CFNumberCreate(nullptr, kCFNumberIntType, &bpe);
	CFNumberRef nf = CFNumberCreate(nullptr, kCFNumberSInt32Type, &fourcc);
	CFDictionarySetValue(props, kIOSurfaceWidth, nw);
	CFDictionarySetValue(props, kIOSurfaceHeight, nh);
	CFDictionarySetValue(props, kIOSurfaceBytesPerElement, nb);
	CFDictionarySetValue(props, kIOSurfacePixelFormat, nf);
	IOSurfaceRef surf = IOSurfaceCreate(props);
	CFRelease(nw);
	CFRelease(nh);
	CFRelease(nb);
	CFRelease(nf);
	CFRelease(props);
	REQUIRE(surf != nullptr);
	REQUIRE(IOSurfaceLock(surf, 0, nullptr) == kIOReturnSuccess);
	uint8_t *base = (uint8_t *)IOSurfaceGetBaseAddress(surf);
	const size_t bpr = IOSurfaceGetBytesPerRow(surf);
	for (int y = 0; y < h; y++) {
		for (int b = 0; b < w * 4; b++) {
			base[y * bpr + b] = (uint8_t)(b * 3 + y * 11);
		}
	}
	IOSurfaceUnlock(surf, 0, nullptr);

	VkExportMetalObjectCreateInfoEXT export_tex = {};
	export_tex.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT;
	export_tex.exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT;
	VkImportMetalIOSurfaceInfoEXT import_surf = {};
	import_surf.sType = VK_STRUCTURE_TYPE_IMPORT_METAL_IO_SURFACE_INFO_EXT;
	import_surf.pNext = &export_tex;
	import_surf.ioSurface = surf;
	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.pNext = &import_surf;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_B8G8R8A8_UNORM;
	ici.extent = {(uint32_t)w, (uint32_t)h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	Image img;
	img.vk = vk;
	img.w = w;
	img.h = h;
	img.format = ici.format;
	REQUIRE(vk->vkCreateImage(vk->device, &ici, nullptr, &img.image) == VK_SUCCESS);
	VkExportMetalTextureInfoEXT et = {};
	et.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT;
	et.image = img.image;
	et.plane = VK_IMAGE_ASPECT_PLANE_0_BIT; // the spec's value for a single-plane image (VUID-06799)
	VkExportMetalObjectsInfoEXT eo = {};
	eo.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT;
	eo.pNext = &et;
	vk->vkExportMetalObjectsEXT(vk->device, &eo);
	REQUIRE(et.mtlTexture != nullptr);
	VkMemoryMetalHandlePropertiesEXT mp = {};
	mp.sType = VK_STRUCTURE_TYPE_MEMORY_METAL_HANDLE_PROPERTIES_EXT;
	REQUIRE(vk->vkGetMemoryMetalHandlePropertiesEXT(vk->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT,
	                                                et.mtlTexture, &mp) == VK_SUCCESS);
	uint32_t type = 0;
	while (type < 32 && (mp.memoryTypeBits & (1u << type)) == 0) {
		type++;
	}
	REQUIRE(type < 32);
	VkImportMemoryMetalHandleInfoEXT im = {};
	im.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_METAL_HANDLE_INFO_EXT;
	im.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT;
	im.handle = et.mtlTexture;
	VkMemoryDedicatedAllocateInfoKHR ded = {};
	ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR;
	ded.pNext = &im;
	ded.image = img.image;
	VkMemoryAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	ai.pNext = &ded;
	ai.allocationSize = 0; // an MTLTexture import with a dedicated image (VUID-VkMemoryAllocateInfo-pNext-10397)
	ai.memoryTypeIndex = type;
	REQUIRE(vk->vkAllocateMemory(vk->device, &ai, nullptr, &img.mem) == VK_SUCCESS);
	REQUIRE(vk->vkBindImageMemory(vk->device, img.image, img.mem, 0) == VK_SUCCESS);

	struct vk_cmd_pool pool = {};
	REQUIRE(vk_cmd_pool_init(vk, &pool, 0) == VK_SUCCESS);
	std::vector<uint8_t> back((size_t)w * h * 4);
	// readback() expects GENERAL: the import carries the IOSurface's bytes in a
	// "preinitialized" sense; an UNDEFINED -> GENERAL transition must not lose them on Metal.
	REQUIRE(run_cmd(vk, &pool, [&](VkCommandBuffer cmd) {
		VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vk_cmd_image_barrier_locked(vk, cmd, img.image, 0, VK_ACCESS_TRANSFER_READ_BIT,
		                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, range);
		return true;
	}));
	REQUIRE(readback(vk, &pool, img, back.data(), back.size()));
	vk_cmd_pool_destroy(vk, &pool);
	bool same = true;
	for (int y = 0; y < h && same; y++) {
		for (int b = 0; b < w * 4 && same; b++) {
			same = back[(size_t)y * w * 4 + b] == (uint8_t)(b * 3 + y * 11);
		}
	}
	CHECK(same);
	SUCCEED("IOSurface import through VK_EXT_metal_objects: " << (same ? "bytes match" : "MISMATCH"));
	CFRelease(surf);
}
#endif

#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_FD)
TEST_CASE("vk glue (Linux): an OPAQUE_FD export re-imports into a second image with the same bytes",
          "[conv_vk_gpu][fd]")
{
	Glue x;
	if (!make_glue(x)) {
		SKIP("no Vulkan device available");
	}
	vk_conv_gpu_caps c{};
	vk_conv_gpu_get_caps(x.g, &c);
	if ((c.memory_kinds & XRT_CONV_MEM_VK_OPAQUE_FD) == 0) {
		SKIP("this device cannot export OPAQUE_FD images");
	}
	struct vk_bundle *vk = x.vk.get();
	const uint32_t w = 40, h = 12;
	auto r = req_of(w, h, XRT_CONV_FMT_RGBA8_UNORM, CONV_IMAGE_USAGE_RENDER_TARGET | CONV_IMAGE_USAGE_EXPORT);
	xrt_conv_image img{};
	REQUIRE(x.ops.alloc(x.ops.ud, &r, &img));
	REQUIRE(img.memory_kind == XRT_CONV_MEM_VK_OPAQUE_FD);
	std::vector<uint8_t> src((size_t)w * h * 4), back(src.size());
	for (size_t i = 0; i < src.size(); i++) {
		src[i] = (uint8_t)(i * 13 + 2);
	}
	REQUIRE(vk_conv_gpu_write(x.g, &img, src.data(), 0));

	// The importer's side: same create info, the fd (dup'ed: the handle is borrowed).
	VkExternalMemoryImageCreateInfo ext = {};
	ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.pNext = &ext;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent = {w, h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	Image imp;
	imp.vk = vk;
	imp.w = w;
	imp.h = h;
	imp.format = ici.format;
	REQUIRE(vk->vkCreateImage(vk->device, &ici, nullptr, &imp.image) == VK_SUCCESS);
	VkMemoryRequirements req;
	vk->vkGetImageMemoryRequirements(vk->device, imp.image, &req);
	VkImportMemoryFdInfoKHR ifd = {};
	ifd.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
	ifd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	ifd.fd = dup((int)img.handle);
	VkMemoryDedicatedAllocateInfoKHR ded = {};
	ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR;
	ded.pNext = &ifd;
	ded.image = imp.image;
	uint32_t type = 0;
	REQUIRE(vk_get_memory_type(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type));
	VkMemoryAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	ai.pNext = &ded;
	ai.allocationSize = img.byte_size;
	ai.memoryTypeIndex = type;
	REQUIRE(vk->vkAllocateMemory(vk->device, &ai, nullptr, &imp.mem) == VK_SUCCESS);
	REQUIRE(vk->vkBindImageMemory(vk->device, imp.image, imp.mem, 0) == VK_SUCCESS);
	struct vk_cmd_pool pool = {};
	REQUIRE(vk_cmd_pool_init(vk, &pool, 0) == VK_SUCCESS);
	// The exporter left it GENERAL; the importer adopts that layout (no discard).
	REQUIRE(readback(vk, &pool, imp, back.data(), back.size()));
	vk_cmd_pool_destroy(vk, &pool);
	CHECK(std::memcmp(src.data(), back.data(), src.size()) == 0);
	x.ops.free(x.ops.ud, &img);
}
#endif

TEST_CASE("vk glue: S6 readback returns an earlier copy's samples, never stalling", "[conv_vk_gpu]")
{
	Glue x;
	if (!make_glue(x)) {
		SKIP("no Vulkan device available");
	}
	vk_conv_gpu_caps c{};
	vk_conv_gpu_get_caps(x.g, &c);
	// A GPU depth image: the mirror usage is always GPU.
	for (uint32_t fmt : {XRT_CONV_FMT_R32_FLOAT, XRT_CONV_FMT_R16_FLOAT}) {
		INFO("fmt " << fmt);
		const uint32_t w = 33, h = 17;
		auto r = req_of(w, h, fmt, CONV_IMAGE_USAGE_MIRROR);
		xrt_conv_image d{};
		REQUIRE(x.ops.alloc(x.ops.ud, &r, &d));
		std::vector<float> v((size_t)w * h);
		std::vector<uint16_t> hv(v.size());
		for (size_t i = 0; i < v.size(); i++) {
			v[i] = quantize_half(1.0f + (float)i / (float)v.size());
			hv[i] = to_half_bits(v[i]);
		}
		REQUIRE(
		    vk_conv_gpu_write(x.g, &d, fmt == XRT_CONV_FMT_R32_FLOAT ? (const void *)v.data() : hv.data(), 0));
		const float *s = nullptr;
		uint32_t sw = 0, sh = 0, stride = 0;
		// First call: issues the copy, nothing fresh yet.
		CHECK_FALSE(x.rb.read(x.rb.ud, &d, &s, &sw, &sh, &stride));
		// A later call sees it (the copy finishes in microseconds).
		REQUIRE(wait_until([&] { return x.rb.read(x.rb.ud, &d, &s, &sw, &sh, &stride); }, 2000));
		REQUIRE(s != nullptr);
		CHECK(sw == w);
		CHECK(sh == h);
		CHECK(stride == w);
		bool same = true;
		for (size_t i = 0; i < v.size() && same; i++) {
			same = s[i] == v[i];
		}
		CHECK(same);
		x.rb.release(x.rb.ud, s);
		// Not every call: the next copy waits out the sample period.
		CHECK_FALSE(x.rb.read(x.rb.ud, &d, &s, &sw, &sh, &stride));
		x.ops.free(x.ops.ud, &d);
	}
	// A CPU image is not the readback's (S6 reads CPU depth directly).
	xrt_conv_image cpu{};
	cpu.memory_kind = XRT_CONV_MEM_CPU;
	cpu.format = XRT_CONV_FMT_R32_FLOAT;
	const float *s = nullptr;
	uint32_t a = 0, b = 0, st = 0;
	CHECK_FALSE(x.rb.read(x.rb.ud, &cpu, &s, &a, &b, &st));
}

namespace {

//! A colour (RGBA8, encodes u in R) + a step depth (affine disparity) on the CPU.
struct CpuScene
{
	static constexpr uint32_t W = 256, H = 144, DW = 64, DH = 36;
	std::vector<uint8_t> color;
	std::vector<float> depth;
	xrt_conv_image c{}, d{};
	xrt_conv_depth_info di{};

	CpuScene()
	{
		color.resize((size_t)W * H * 4);
		for (uint32_t y = 0; y < H; y++) {
			for (uint32_t x = 0; x < W; x++) {
				uint8_t *p = &color[((size_t)y * W + x) * 4];
				p[0] = (uint8_t)std::lround((x + 0.5) / W * 255.0);
				p[1] = (uint8_t)std::lround((1.0 - (y + 0.5) / H) * 255.0);
				p[2] = (x >= 96 && x < 160 && y >= 48 && y < 96) ? 255 : 0;
				p[3] = 255;
			}
		}
		depth.assign((size_t)DW * DH, 0.0f);
		for (uint32_t y = 12; y < 24; y++) {
			for (uint32_t x = 24; x < 40; x++) {
				depth[(size_t)y * DW + x] = 1.0f;
			}
		}
		c.memory_kind = XRT_CONV_MEM_CPU;
		c.cpu_ptr = color.data();
		c.byte_size = color.size();
		c.width = W;
		c.height = H;
		c.format = XRT_CONV_FMT_RGBA8_UNORM;
		c.row_stride = W * 4;
		c.import_key = (1ull << 63) | 11;
		d.memory_kind = XRT_CONV_MEM_CPU;
		d.cpu_ptr = depth.data();
		d.byte_size = depth.size() * 4;
		d.width = DW;
		d.height = DH;
		d.format = XRT_CONV_FMT_R32_FLOAT;
		d.row_stride = DW * 4;
		d.import_key = (1ull << 63) | 12;
		di.kind = XRT_CONV_DEPTH_AFFINE_DISPARITY;
		di.value_scale = 1.0f;
		di.invalid_sample = NAN;
	}

	conv_viewsyn_request
	request(const xrt_conv_image *target, uint32_t layout) const
	{
		conv_viewsyn_request q{};
		q.color = &c;
		q.depth = &d;
		q.depth_info = &di;
		q.view_count = 2;
		q.viewpoints_m[0][0] = -0.0315f;
		q.viewpoints_m[1][0] = 0.0315f;
		q.viewpoints_m[0][2] = q.viewpoints_m[1][2] = 0.6f;
		q.rect_width_m = 0.32f;
		q.rect_height_m = 0.18f;
		q.range_near = 1.0f;
		q.range_far = 0.0f;
		q.convergence = 0.5f;
		q.budget = 0.1f;
		q.rear_cap_m = -1.0f;
		q.target = target;
		q.layout = layout;
		q.new_content = true;
		return q;
	}
};

} // namespace

TEST_CASE("vk glue: S5 on CPU colour + CPU depth, into a GPU target and into a CPU target", "[conv_vk_gpu]")
{
	Glue x;
	if (!make_glue(x)) {
		SKIP("no Vulkan device available");
	}
	CpuScene sc;
	const uint32_t W = CpuScene::W, H = CpuScene::H;

	// GPU target (the composed views, ROW).
	auto r = req_of(2 * W, H, XRT_CONV_FMT_RGBA8_UNORM, CONV_IMAGE_USAGE_RENDER_TARGET | CONV_IMAGE_USAGE_EXPORT, 1,
	                0, 42);
	xrt_conv_image t{};
	REQUIRE(x.ops.alloc(x.ops.ud, &r, &t));
	conv_viewsyn_request q = sc.request(&t, XRT_CONV_VIEWS_ROW);
	REQUIRE(x.vs.render(x.vs.ud, &q));
	// macOS (step E): the render was handed over asynchronously, on the glue's
	// MTLSharedEvent; the read below retires it first (same bytes either way).
	{
		vk_conv_gpu_caps caps{};
		vk_conv_gpu_get_caps(x.g, &caps);
		vk_conv_gpu_image_ref ref{};
		REQUIRE(vk_conv_gpu_lookup(x.g, &t, &ref));
		vk_conv_gpu_stats st{};
		vk_conv_gpu_get_stats(x.g, &st);
		if (caps.async_s5) {
			CHECK(ref.mtl_shared_event != nullptr);
			CHECK(ref.event_value == 1u);
			CHECK(st.async_s5 == 1u);
		} else {
			CHECK(ref.mtl_shared_event == nullptr);
			CHECK(st.async_s5 == 0u);
		}
	}
	std::vector<uint8_t> gpu((size_t)2 * W * H * 4);
	REQUIRE(vk_conv_gpu_read(x.g, &t, gpu.data(), 0));
	CHECK(vk_conv_gpu_viewsyn_prepass_count(x.g) == 1u);

	// Each view: the far ground keeps its own u away from the square (taper
	// band aside), and the near square moved in opposite directions.
	auto blue_left_edge = [&](const std::vector<uint8_t> &img, uint32_t view, uint32_t row_w) {
		const uint32_t y = 72;
		for (uint32_t x = 1; x < W; x++) {
			const uint8_t b0 = img[((size_t)y * row_w + view * W + x - 1) * 4 + 2];
			const uint8_t b1 = img[((size_t)y * row_w + view * W + x) * 4 + 2];
			if (b0 < 128 && b1 >= 128) {
				return (int)x;
			}
		}
		return -1;
	};
	const int e0 = blue_left_edge(gpu, 0, 2 * W), e1 = blue_left_edge(gpu, 1, 2 * W);
	INFO("square's left edge: view 0 at " << e0 << ", view 1 at " << e1 << " (source 96)");
	REQUIRE(e0 > 0);
	REQUIRE(e1 > 0);
	CHECK(e0 > 96); // the left eye sees the near square shifted right
	CHECK(e1 < 96);
	const uint8_t *ground = &gpu[((size_t)10 * 2 * W + 40) * 4]; // view 0, near the top-left, far ground
	CHECK(std::abs((int)ground[0] - (int)std::lround(40.5 / W * 255.0)) <= 6);

	// The same synthesis into a CPU target (the scratch + readback path) is
	// the same pixels: the scratch target goes through the same blit.
	std::vector<uint8_t> cpu_t(gpu.size(), 0);
	xrt_conv_image ct{};
	ct.memory_kind = XRT_CONV_MEM_CPU;
	ct.cpu_ptr = cpu_t.data();
	ct.byte_size = cpu_t.size();
	ct.width = 2 * W;
	ct.height = H;
	ct.format = XRT_CONV_FMT_RGBA8_UNORM;
	ct.row_stride = 2 * W * 4;
	ct.import_key = (1ull << 63) | 13;
	// Needs a glue image of the stream to hang the scratch on: the colour.
	auto rc = req_of(W, H, XRT_CONV_FMT_RGBA8_UNORM, CONV_IMAGE_USAGE_MIRROR, 1, 0, 43);
	xrt_conv_image gc{};
	REQUIRE(x.ops.alloc(x.ops.ud, &rc, &gc));
	REQUIRE(vk_conv_gpu_write(x.g, &gc, sc.color.data(), 0));
	conv_viewsyn_request q2 = sc.request(&ct, XRT_CONV_VIEWS_ROW);
	q2.color = &gc;
	REQUIRE(x.vs.render(x.vs.ud, &q2));
	CHECK(std::memcmp(cpu_t.data(), gpu.data(), gpu.size()) == 0);

	// A request with only CPU images has no stream to hang scratch on: refused.
	conv_viewsyn_request q3 = sc.request(&ct, XRT_CONV_VIEWS_ROW);
	CHECK_FALSE(x.vs.render(x.vs.ud, &q3));

	x.ops.free(x.ops.ud, &gc);
	x.ops.free(x.ops.ud, &t);
	CHECK(vk_conv_gpu_image_count(x.g) == 0u);
}

namespace {

//! The service trampoline's shape: the glue first, CPU memory when it declines.
struct Tramp
{
	conv_image_alloc_ops gpu{};
	std::atomic<uint64_t> key{1};
	std::atomic<int> cpu_live{0};
	std::atomic<int> gpu_allocs{0};
};

bool
tr_alloc(void *ud, const conv_image_request *req, xrt_conv_image *out)
{
	auto *t = static_cast<Tramp *>(ud);
	if (t->gpu.alloc(t->gpu.ud, req, out)) {
		t->gpu_allocs++;
		return true;
	}
	if (req->import_memory_kinds != 0 && (req->import_memory_kinds & XRT_CONV_MEM_CPU) == 0) {
		return false;
	}
	uint32_t bpp = req->format == XRT_CONV_FMT_R8_UNORM                                               ? 1
	               : (req->format == XRT_CONV_FMT_R16_FLOAT || req->format == XRT_CONV_FMT_R16_UNORM) ? 2
	               : req->format == XRT_CONV_FMT_RGBA16_FLOAT                                         ? 8
	                                                                                                  : 4;
	const uint32_t layers = req->array_layers ? req->array_layers : 1;
	const uint64_t size = (uint64_t)req->width * req->height * bpp * layers;
	void *p = std::calloc(1, (size_t)size);
	if (p == nullptr) {
		return false;
	}
	std::memset(out, 0, sizeof(*out));
	out->memory_kind = XRT_CONV_MEM_CPU;
	out->cpu_ptr = p;
	out->byte_size = size;
	out->width = req->width;
	out->height = req->height;
	out->format = req->format;
	out->row_stride = req->width * bpp;
	out->import_key = (t->key++) | (1ull << 63);
	out->color_space = req->role == 0 ? XRT_CONV_CS_SRGB : XRT_CONV_CS_DATA;
	t->cpu_live++;
	return true;
}

void
tr_free(void *ud, xrt_conv_image *img)
{
	auto *t = static_cast<Tramp *>(ud);
	if (img->memory_kind == XRT_CONV_MEM_CPU && (img->import_key >> 63) != 0) {
		std::free(img->cpu_ptr);
		img->cpu_ptr = nullptr;
		t->cpu_live--;
		return;
	}
	t->gpu.free(t->gpu.ud, img);
}

bool
tr_upload(void *ud, const xrt_conv_image *src, const xrt_conv_image *dst)
{
	auto *t = static_cast<Tramp *>(ud);
	return t->gpu.upload(t->gpu.ud, src, dst);
}

} // namespace

TEST_CASE("vk glue: a composed VIEWS_MONO conversion through conv_core (CPU depth provider, GPU views)",
          "[conv_vk_gpu]")
{
	Glue x;
	if (!make_glue(x)) {
		SKIP("no Vulkan device available");
	}
	conv_fake_provider f;
	conv_fake_init(&f, "depth-only");
	conv_fake_cap *c =
	    conv_fake_add_cap(&f, 2, XRT_CONV_TASK_DEPTH_VIDEO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_DEPTH, 0);
	c->state = XRT_CONV_STATE_READY;
	c->cap.depth_kind = XRT_CONV_DEPTH_METRIC;

	Tramp tr;
	tr.gpu = x.ops;
	{
		conv_core_config cfg{};
		cfg.alloc.alloc = tr_alloc;
		cfg.alloc.free = tr_free;
		cfg.alloc.upload = tr_upload;
		cfg.alloc.ud = &tr;
		cfg.viewsyn = x.vs;
		cfg.readback = x.rb;
		cfg.prewarm_on = CONV_PREWARM_FIRST_CREATE;
		cfg.prewarm_task_mask = 1ull << 63;
		cfg.rear_cap_m = -1.0f;
		cfg.tick_ms = 10;
		conv_core *core = nullptr;
		REQUIRE(conv_core_create(&cfg, &core) == XRT_SUCCESS);
		conv_provider_desc d{};
		d.id = f.id;
		d.probe_order = 100;
		d.enabled = true;
		d.iface = &f.iface;
		d.inst = conv_fake_inst(&f);
		REQUIRE(conv_core_add_provider(core, &d) >= 0);

		xrt_conv_wire_stream_desc sd{};
		sd.struct_size = sizeof(sd);
		sd.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
		sd.task = XRT_CONV_TASK_VIEWS_MONO;
		sd.outputs = XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH;
		sd.view_count = 2;
		sd.views_layout = XRT_CONV_VIEWS_ROW;
		sd.input_kind = XRT_CONV_INPUT_MONO;
		xrt_conv_wire_stream_created cr{};
		REQUIRE(conv_core_stream_create(core, 1, &sd, nullptr, &cr) == XRT_SUCCESS);
		CHECK((cr.desc_flags & XRT_CONV_WIRE_DESC_COMPOSED) != 0u);

		for (int frame = 0; frame < 3; frame++) {
			conv_submit_layout lay{};
			lay.image_count = 1;
			lay.width[0] = 64;
			lay.height[0] = 32;
			lay.format[0] = XRT_CONV_FMT_RGBA8_UNORM;
			lay.color_space[0] = XRT_CONV_CS_SRGB;
			lay.role[0] = XRT_CONV_IMG_COLOR;
			conv_submit_slot slot{};
			REQUIRE(conv_core_stream_begin_submit(core, cr.stream_id, &lay, &slot) == XRT_SUCCESS);
			// The input is CPU memory (the depth provider imports CPU only; the provider mask is 0).
			REQUIRE(slot.images[0].memory_kind == XRT_CONV_MEM_CPU);
			std::memset(slot.images[0].cpu_ptr, 0x80, (size_t)slot.images[0].byte_size);
			conv_frame_submit fr{};
			fr.source_time = (int64_t)now_ns();
			fr.rect_valid = true;
			fr.rect_w_m = 0.30f;
			fr.rect_h_m = 0.20f;
			uint64_t fid = 0;
			REQUIRE(conv_core_stream_commit_submit(core, cr.stream_id, slot.slot, &fr, &fid) ==
			        XRT_SUCCESS);
			conv_result res{};
			REQUIRE(wait_until(
			    [&] { return conv_core_stream_acquire(core, cr.stream_id, true, &res) == XRT_SUCCESS; },
			    3000));
			INFO("frame " << frame);
			CHECK(res.written == (XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH));
			CHECK(res.views_info.view_count == 2u);
			// The composed views are the glue's GPU image; the provider wrote its
			// depth on the CPU lane and the core published it through the
			// glue's GPU mirror (#2030 upload).
			CHECK(res.views.memory_kind != XRT_CONV_MEM_CPU);
			CHECK(res.depth.memory_kind != XRT_CONV_MEM_CPU);
			std::vector<float> dpx((size_t)res.depth.width * res.depth.height);
			REQUIRE(vk_conv_gpu_read(x.g, &res.depth, dpx.data(), 0));
			const auto mm = std::minmax_element(dpx.begin(), dpx.end());
			CHECK(*mm.first >= 1.0f); // the fake provider's ramp, 1..2 m
			CHECK(*mm.second <= 2.0f);
			CHECK(*mm.second > *mm.first);
			CHECK(res.views.width == 128u);
			std::vector<uint8_t> px((size_t)res.views.width * res.views.height * 4);
			REQUIRE(vk_conv_gpu_read(x.g, &res.views, px.data(), 0));
			// A flat grey input synthesises flat grey views (every pixel drawn).
			size_t off = 0;
			for (size_t i = 0; i < px.size(); i += 4) {
				off += (std::abs((int)px[i] - 0x80) > 1 || px[i + 3] != 255) ? 1 : 0;
			}
			CHECK(off == 0u);
			conv_core_stream_release(core, cr.stream_id, res.ring_slot);
		}
		conv_stream_stats st{};
		REQUIRE(conv_core_stream_get_stats(core, cr.stream_id, &st) == XRT_SUCCESS);
		CHECK(st.converted >= 3u);
		CHECK(st.failed == 0u);
		CHECK(st.composed);
		CHECK(tr.gpu_allocs >= 1);
		CHECK(vk_conv_gpu_viewsyn_prepass_count(x.g) >= 3u);
		REQUIRE(conv_core_stream_destroy(core, cr.stream_id) == XRT_SUCCESS);
		conv_core_destroy(&core);
	}
	CHECK(tr.cpu_live == 0);
	CHECK(vk_conv_gpu_image_count(x.g) == 0u);
	conv_fake_fini(&f);
}

namespace {

bool
hook_write(void *ud, const xrt_conv_image *dst, const void *px, uint32_t pitch)
{
	return vk_conv_gpu_write(static_cast<vk_conv_gpu *>(ud), dst, px, pitch);
}

bool
hook_read(void *ud, const xrt_conv_image *src, void *px, uint32_t pitch)
{
	return vk_conv_gpu_read(static_cast<vk_conv_gpu *>(ud), src, px, pitch);
}

} // namespace

TEST_CASE("vk glue under the CPU I/O table: one POSIX table, composed views exported as CPU bytes", "[conv_vk_gpu]")
{
	// The POSIX composition (ipc_server_conv_vk.c): the CPU I/O glue is the one
	// I/O table (client CPU bytes in and out, cpu_only); the Vulkan glue owns
	// allocation + S5 + S6 and lends the table its CPU <-> image copies.
	ipc_server_conv_io_ops cpu_io;
	if (!ipc_server_conv_cpu_io_get(&cpu_io)) {
		SKIP("no CPU buffer transport on this platform");
	}
	Glue x;
	if (!make_glue(x)) {
		SKIP("no Vulkan device available");
	}
	conv_fake_provider f;
	conv_fake_init(&f, "depth-only");
	conv_fake_cap *c =
	    conv_fake_add_cap(&f, 2, XRT_CONV_TASK_DEPTH_VIDEO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_DEPTH, 0);
	c->state = XRT_CONV_STATE_READY;
	c->cap.depth_kind = XRT_CONV_DEPTH_METRIC;

	Tramp tr;
	tr.gpu = x.ops;
	conv_core_config cfg{};
	cfg.alloc.alloc = tr_alloc;
	cfg.alloc.free = tr_free;
	cfg.alloc.upload = tr_upload;
	cfg.alloc.ud = &tr;
	cfg.prewarm_on = CONV_PREWARM_FIRST_CREATE;
	cfg.prewarm_task_mask = 1ull << 63;
	cfg.rear_cap_m = -1.0f;
	cfg.tick_ms = 10;
	conv_core *core = nullptr;
	REQUIRE(conv_core_create(&cfg, &core) == XRT_SUCCESS);
	ipc_server_conv *sc = nullptr;
	REQUIRE(ipc_server_conv_create(core, &sc) == XRT_SUCCESS);
	// Service start: the CPU table alone (service_conversion.c).
	ipc_server_conv_set_io(sc, &cpu_io);
	CHECK(ipc_server_conv_v4_image_kinds(sc, false) == (1ull << IPC_SERVER_CONV_IMAGE_KIND_CPU));

	// The attach, in ipc_server_conv_vk.c's order: copies, then the GPU side
	// with the CPU table as its io (the route stays the CPU route).
	ipc_server_conv_cpu_gpu copies{hook_write, hook_read, x.g};
	ipc_server_conv_cpu_io_set_gpu(&copies);
	ipc_server_conv_gpu gpu{};
	gpu.alloc = x.ops; // the service hook would swap the trampoline's GPU half; Tramp already has it
	gpu.viewsyn = x.vs;
	gpu.readback = x.rb;
	gpu.io = cpu_io;
	ipc_server_conv_install_gpu(sc, &gpu);
	CHECK(ipc_server_conv_v4_image_kinds(sc, false) == (1ull << IPC_SERVER_CONV_IMAGE_KIND_CPU));
	CHECK(ipc_server_conv_v4_image_kinds(sc, true) == (1ull << IPC_SERVER_CONV_IMAGE_KIND_CPU));
	CHECK_FALSE(ipc_server_conv_routes_create(sc, XRT_DP_LIFT_MODE_SBS, 0)); // cpu_only: no v3 route

	conv_provider_desc d{};
	d.id = f.id;
	d.probe_order = 100;
	d.enabled = true;
	d.iface = &f.iface;
	d.inst = conv_fake_inst(&f);
	REQUIRE(conv_core_add_provider(core, &d) >= 0);
	xrt_conv_wire_stream_desc sd{};
	sd.struct_size = sizeof(sd);
	sd.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
	sd.task = XRT_CONV_TASK_VIEWS_MONO;
	sd.outputs = XRT_CONV_OUT_VIEWS;
	sd.view_count = 2;
	sd.views_layout = XRT_CONV_VIEWS_ROW;
	sd.input_kind = XRT_CONV_INPUT_MONO;
	xrt_conv_wire_stream_created cr{};
	REQUIRE(conv_core_stream_create(core, 1, &sd, nullptr, &cr) == XRT_SUCCESS);
	CHECK((cr.desc_flags & XRT_CONV_WIRE_DESC_COMPOSED) != 0u);

	// S0 through the CPU table: client bytes (BGRA, flat 0x60 grey) into the slot.
	conv_submit_layout lay{};
	lay.image_count = 1;
	lay.width[0] = 64;
	lay.height[0] = 32;
	lay.format[0] = XRT_CONV_FMT_RGBA8_UNORM;
	lay.color_space[0] = XRT_CONV_CS_SRGB;
	lay.role[0] = XRT_CONV_IMG_COLOR;
	conv_submit_slot slot{};
	REQUIRE(conv_core_stream_begin_submit(core, cr.stream_id, &lay, &slot) == XRT_SUCCESS);
	std::vector<uint8_t> client((size_t)64 * 32 * 4, 0x60);
	xrt_graphics_buffer_handle_t ch = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	REQUIRE(ipc_cpu_buffer_create(client.size(), &ch));
	{
		struct ipc_cpu_buffer_map m;
		REQUIRE(ipc_cpu_buffer_map(ch, client.size(), true, &m));
		std::memcpy(m.ptr, client.data(), client.size());
		ipc_cpu_buffer_unmap(&m);
	}
	conv_input_import in{};
	in.kind = CONV_INPUT_CPU;
	in.handle = ch;
	in.sync_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	in.w = 64;
	in.h = 32;
	in.cpu_row_pitch = 64 * 4;
	in.cpu_bgra = true;
	REQUIRE(cpu_io.import_input_ex(cpu_io.ud, cr.stream_id, &in, &slot) == XRT_SUCCESS);
	conv_frame_submit fr{};
	fr.source_time = (int64_t)now_ns();
	fr.rect_valid = true;
	fr.rect_w_m = 0.30f;
	fr.rect_h_m = 0.20f;
	uint64_t fid = 0;
	REQUIRE(conv_core_stream_commit_submit(core, cr.stream_id, slot.slot, &fr, &fid) == XRT_SUCCESS);
	conv_result res{};
	REQUIRE(
	    wait_until([&] { return conv_core_stream_acquire(core, cr.stream_id, true, &res) == XRT_SUCCESS; }, 3000));
	REQUIRE(res.views.memory_kind != XRT_CONV_MEM_CPU); // the glue's composed views

	// S8 through the CPU table: the GPU views read back into the export section.
	ipc_server_conv_published pub{};
	REQUIRE(cpu_io.publish_result(cpu_io.ud, cr.stream_id, &res, &pub) == XRT_SUCCESS);
	conv_core_stream_release(core, cr.stream_id, res.ring_slot);
	CHECK(pub.width == 128u);
	CHECK(pub.height == 32u);
	CHECK(pub.realloc);
	xrt_graphics_buffer_handle_t eh = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	uint32_t w = 0, h = 0, fmt = 0;
	uint64_t bytes = 0;
	bool realloc = false;
	REQUIRE(cpu_io.export_cpu(cpu_io.ud, cr.stream_id, false, &eh, &w, &h, &fmt, &bytes, &realloc));
	REQUIRE(bytes == 128u * 32u * 4u);
	{
		struct ipc_cpu_buffer_map m;
		REQUIRE(ipc_cpu_buffer_map(eh, bytes, false, &m));
		const auto *p = static_cast<const uint8_t *>(m.ptr);
		size_t off = 0;
		for (size_t i = 0; i < bytes; i += 4) {
			off += (std::abs((int)p[i] - 0x60) > 1 || p[i + 3] != 255) ? 1 : 0;
		}
		CHECK(off == 0u); // flat grey in, flat grey views out, every pixel drawn
		ipc_cpu_buffer_unmap(&m);
	}

	// A GPU slot image (a provider given GPU inputs) is filled with the glue's write.
	vk_conv_gpu_caps caps{};
	vk_conv_gpu_get_caps(x.g, &caps);
	if ((caps.memory_kinds & ~XRT_CONV_MEM_CPU) != 0) {
		vk_conv_gpu_set_provider_kinds(x.g, caps.memory_kinds);
		auto r = req_of(8, 4, XRT_CONV_FMT_RGBA8_UNORM, CONV_IMAGE_USAGE_INPUT, 1, caps.memory_kinds);
		conv_submit_slot gs{};
		gs.image_count = 1;
		REQUIRE(x.ops.alloc(x.ops.ud, &r, &gs.images[0]));
		std::vector<uint8_t> src((size_t)8 * 4 * 4);
		for (size_t i = 0; i < src.size(); i++) {
			src[i] = (uint8_t)i;
		}
		xrt_graphics_buffer_handle_t gh = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
		REQUIRE(ipc_cpu_buffer_create(src.size(), &gh));
		{
			struct ipc_cpu_buffer_map m;
			REQUIRE(ipc_cpu_buffer_map(gh, src.size(), true, &m));
			std::memcpy(m.ptr, src.data(), src.size());
			ipc_cpu_buffer_unmap(&m);
		}
		conv_input_import gi{};
		gi.kind = CONV_INPUT_CPU;
		gi.handle = gh;
		gi.sync_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
		gi.w = 8;
		gi.h = 4;
		gi.cpu_row_pitch = 8 * 4;
		REQUIRE(cpu_io.import_input_ex(cpu_io.ud, 99, &gi, &gs) == XRT_SUCCESS);
		std::vector<uint8_t> back(src.size());
		REQUIRE(vk_conv_gpu_read(x.g, &gs.images[0], back.data(), 0));
		CHECK(back == src);
		x.ops.free(x.ops.ud, &gs.images[0]);
		vk_conv_gpu_set_provider_kinds(x.g, 0);
	}

	// The detach, in ipc_server_conv_vk.c's order; the CPU route outlives it.
	cpu_io.stream_gone(cpu_io.ud, cr.stream_id);
	REQUIRE(conv_core_stream_destroy(core, cr.stream_id) == XRT_SUCCESS);
	ipc_server_conv_install_gpu(sc, nullptr);
	ipc_server_conv_cpu_io_set_gpu(nullptr);
	ipc_server_conv_set_io(sc, &cpu_io);
	CHECK(ipc_server_conv_v4_image_kinds(sc, false) == (1ull << IPC_SERVER_CONV_IMAGE_KIND_CPU));
	ipc_server_conv_destroy(&sc);
	conv_core_destroy(&core);
	CHECK(tr.cpu_live == 0);
	CHECK(vk_conv_gpu_image_count(x.g) == 0u);
	conv_fake_fini(&f);
}

/*
 *
 * Step E: the client GPU transport (IOSurface on macOS; OPAQUE_FD / dma-buf on
 * Linux) through the CPU I/O table, with the glue's hooks.
 *
 */

namespace {

bool
t_import(void *ud, const conv_input_import *in, const xrt_conv_image *dst)
{
	return vk_conv_gpu_import_input(static_cast<vk_conv_gpu *>(ud), in, dst);
}

bool
t_export_alloc(void *ud, uint32_t kind, uint32_t fmt, uint32_t w, uint32_t h, uint64_t sid, xrt_conv_image *out)
{
	return vk_conv_gpu_export_alloc(static_cast<vk_conv_gpu *>(ud), kind, fmt, w, h, sid, out);
}

void
t_export_free(void *ud, xrt_conv_image *img)
{
	conv_image_alloc_ops ops{};
	vk_conv_gpu_alloc_ops(static_cast<vk_conv_gpu *>(ud), &ops);
	ops.free(ops.ud, img);
}

bool
t_copy(void *ud, const xrt_conv_image *src, const xrt_conv_image *dst)
{
	return vk_conv_gpu_copy(static_cast<vk_conv_gpu *>(ud), src, dst);
}

//! The POSIX service's I/O composition (ipc_server_conv_vk.c) without a core.
struct TransportRig
{
	Glue x;
	Tramp tr;
	ipc_server_conv_io_ops cpu_io{};
	bool ok = false;

	bool
	init()
	{
		if (!ipc_server_conv_cpu_io_get(&cpu_io) || !make_glue(x)) {
			return false;
		}
		tr.gpu = x.ops;
		ipc_server_conv_cpu_gpu h{};
		h.write = hook_write;
		h.read = hook_read;
		h.ud = x.g;
		h.import_kinds = vk_conv_gpu_import_kinds(x.g);
		h.export_kinds = vk_conv_gpu_export_kinds(x.g);
		h.import_copy = t_import;
		h.export_alloc = t_export_alloc;
		h.export_free = t_export_free;
		h.copy = t_copy;
		ipc_server_conv_cpu_io_set_gpu(&h);
		ok = true;
		return true;
	}

	~TransportRig()
	{
		if (ok) {
			ipc_server_conv_cpu_io_set_gpu(nullptr);
		}
	}

	vk_conv_gpu_stats
	stats()
	{
		vk_conv_gpu_stats s{};
		vk_conv_gpu_get_stats(x.g, &s);
		return s;
	}
};

//! Bytes that differ (0 = byte-exact); logs the first difference.
size_t
bytes_off(const std::vector<uint8_t> &got, const std::vector<uint8_t> &want)
{
	if (got.size() != want.size()) {
		UNSCOPED_INFO("size " << got.size() << " != " << want.size());
		return SIZE_MAX;
	}
	size_t n = 0;
	for (size_t i = 0; i < got.size(); i++) {
		if (got[i] != want[i]) {
			if (n == 0) {
				UNSCOPED_INFO("first difference at byte " << i << " (px " << i / 4 << " ch " << i % 4
				                                          << "): got " << (int)got[i] << " want "
				                                          << (int)want[i]);
			}
			n++;
		}
	}
	return n;
}

//! A tightly packed RGBA8 pattern.
std::vector<uint8_t>
pattern(uint32_t w, uint32_t h, uint32_t salt)
{
	std::vector<uint8_t> px((size_t)w * h * 4);
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			uint8_t *p = &px[((size_t)y * w + x) * 4];
			p[0] = (uint8_t)(x * 7 + salt);
			p[1] = (uint8_t)(y * 13 + salt * 3);
			p[2] = (uint8_t)(x * y + salt);
			p[3] = (uint8_t)(255 - x);
		}
	}
	return px;
}

//! Region (@p x0, @p y0, @p w x @p h) of a @p sw wide RGBA8 / BGRA8 source, as RGBA8.
std::vector<uint8_t>
expect_region(const std::vector<uint8_t> &src, uint32_t sw, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, bool bgra)
{
	std::vector<uint8_t> out((size_t)w * h * 4);
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			const uint8_t *s = &src[((size_t)(y0 + y) * sw + x0 + x) * 4];
			uint8_t *d = &out[((size_t)y * w + x) * 4];
			d[0] = bgra ? s[2] : s[0];
			d[1] = s[1];
			d[2] = bgra ? s[0] : s[2];
			d[3] = s[3];
		}
	}
	return out;
}

} // namespace

#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_METAL)
namespace {

//! A global IOSurface (as an app hands one in: the transport carries its ID) holding @p px.
IOSurfaceRef
make_global_iosurface(uint32_t w, uint32_t h, uint32_t fourcc, const std::vector<uint8_t> &px)
{
	CFMutableDictionaryRef props =
	    CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	int iw = (int)w, ih = (int)h, bpe = 4;
	CFNumberRef nw = CFNumberCreate(nullptr, kCFNumberIntType, &iw);
	CFNumberRef nh = CFNumberCreate(nullptr, kCFNumberIntType, &ih);
	CFNumberRef nb = CFNumberCreate(nullptr, kCFNumberIntType, &bpe);
	CFNumberRef nf = CFNumberCreate(nullptr, kCFNumberSInt32Type, &fourcc);
	CFDictionarySetValue(props, kIOSurfaceWidth, nw);
	CFDictionarySetValue(props, kIOSurfaceHeight, nh);
	CFDictionarySetValue(props, kIOSurfaceBytesPerElement, nb);
	CFDictionarySetValue(props, kIOSurfacePixelFormat, nf);
	CFDictionarySetValue(props, CFSTR("IOSurfaceIsGlobal"), kCFBooleanTrue);
	IOSurfaceRef s = IOSurfaceCreate(props);
	CFRelease(nw);
	CFRelease(nh);
	CFRelease(nb);
	CFRelease(nf);
	CFRelease(props);
	if (s == nullptr) {
		return nullptr;
	}
	IOSurfaceLock(s, 0, nullptr);
	uint8_t *base = (uint8_t *)IOSurfaceGetBaseAddress(s);
	const size_t bpr = IOSurfaceGetBytesPerRow(s);
	for (uint32_t y = 0; y < h; y++) {
		memcpy(base + y * bpr, &px[(size_t)y * w * 4], (size_t)w * 4);
	}
	IOSurfaceUnlock(s, 0, nullptr);
	return s;
}

//! An app's import of an IOSurface: VkImportMetalIOSurfaceInfoEXT, then the MTLTexture memory import.
bool
import_iosurface_app(struct vk_bundle *vk, IOSurfaceRef surf, VkFormat format, Image &img)
{
	VkExportMetalObjectCreateInfoEXT export_tex = {};
	export_tex.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT;
	export_tex.exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT;
	VkImportMetalIOSurfaceInfoEXT imp = {};
	imp.sType = VK_STRUCTURE_TYPE_IMPORT_METAL_IO_SURFACE_INFO_EXT;
	imp.pNext = &export_tex;
	imp.ioSurface = surf;
	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.pNext = &imp;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = format;
	ici.extent = {(uint32_t)IOSurfaceGetWidth(surf), (uint32_t)IOSurfaceGetHeight(surf), 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	img.vk = vk;
	img.w = ici.extent.width;
	img.h = ici.extent.height;
	img.format = format;
	if (vk->vkCreateImage(vk->device, &ici, nullptr, &img.image) != VK_SUCCESS) {
		return false;
	}
	VkExportMetalTextureInfoEXT et = {};
	et.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT;
	et.image = img.image;
	et.plane = VK_IMAGE_ASPECT_PLANE_0_BIT;
	VkExportMetalObjectsInfoEXT eo = {};
	eo.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT;
	eo.pNext = &et;
	vk->vkExportMetalObjectsEXT(vk->device, &eo);
	VkMemoryMetalHandlePropertiesEXT mp = {};
	mp.sType = VK_STRUCTURE_TYPE_MEMORY_METAL_HANDLE_PROPERTIES_EXT;
	if (et.mtlTexture == nullptr ||
	    vk->vkGetMemoryMetalHandlePropertiesEXT(vk->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT,
	                                            et.mtlTexture, &mp) != VK_SUCCESS) {
		return false;
	}
	uint32_t type = 0;
	while (type < 32 && (mp.memoryTypeBits & (1u << type)) == 0) {
		type++;
	}
	VkImportMemoryMetalHandleInfoEXT im = {};
	im.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_METAL_HANDLE_INFO_EXT;
	im.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT;
	im.handle = et.mtlTexture;
	VkMemoryDedicatedAllocateInfoKHR ded = {};
	ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR;
	ded.pNext = &im;
	ded.image = img.image;
	VkMemoryAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	ai.pNext = &ded;
	ai.allocationSize = 0;
	ai.memoryTypeIndex = type;
	return type < 32 && vk->vkAllocateMemory(vk->device, &ai, nullptr, &img.mem) == VK_SUCCESS &&
	       vk->vkBindImageMemory(vk->device, img.image, img.mem, 0) == VK_SUCCESS;
}

//! The IOSurface's bytes, tightly packed.
std::vector<uint8_t>
iosurface_bytes(IOSurfaceRef s)
{
	const size_t w = IOSurfaceGetWidth(s), h = IOSurfaceGetHeight(s), bpr = IOSurfaceGetBytesPerRow(s);
	std::vector<uint8_t> out(w * h * 4);
	IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr);
	const uint8_t *base = (const uint8_t *)IOSurfaceGetBaseAddress(s);
	for (size_t y = 0; y < h; y++) {
		memcpy(&out[y * w * 4], base + y * bpr, w * 4);
	}
	IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
	return out;
}

} // namespace

TEST_CASE("transport (macOS): an IOSurface submit fills the slot -- a GPU slot with no host copy, a CPU slot with one",
          "[conv_vk_gpu][transport][iosurface]")
{
	TransportRig rig;
	if (!rig.init()) {
		SKIP("no Vulkan device / CPU transport");
	}
	if ((vk_conv_gpu_import_kinds(rig.x.g) & XRT_CONV_MEM_IOSURFACE) == 0) {
		SKIP("VK_EXT_metal_objects / VK_EXT_external_memory_metal not available");
	}
	CHECK(ipc_server_conv_cpu_io_gpu_image_kinds(false) == (1ull << IPC_SERVER_CONV_IMAGE_KIND_IOSURFACE));
	CHECK(ipc_server_conv_cpu_io_gpu_image_kinds(true) == (1ull << IPC_SERVER_CONV_IMAGE_KIND_IOSURFACE));

	// A 40x24 BGRA IOSurface; the region (4,2 32x16) is what converts.
	const uint32_t W = 40, H = 24, X = 4, Y = 2, RW = 32, RH = 16;
	const std::vector<uint8_t> src = pattern(W, H, 5);
	const std::vector<uint8_t> want = expect_region(src, W, X, Y, RW, RH, /*bgra*/ true);

	auto submit = [&](const xrt_conv_image &dst) {
		IOSurfaceRef s = make_global_iosurface(W, H, 'BGRA', src);
		REQUIRE(s != nullptr);
		conv_input_import in{};
		in.kind = CONV_INPUT_IOSURFACE;
		in.handle = (xrt_graphics_buffer_handle_t)s; // the table releases it, as the transport's lookup ref
		in.sync_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
		in.x = X;
		in.y = Y;
		in.w = RW;
		in.h = RH;
		in.img_w = W;
		in.img_h = H;
		in.img_format = XRT_CONV_FMT_BGRA8_UNORM;
		conv_submit_slot slot{};
		slot.image_count = 1;
		slot.images[0] = dst;
		return rig.cpu_io.import_input_ex(rig.cpu_io.ud, 1, &in, &slot);
	};

	SECTION("CPU slot (a provider on the CPU lane): one GPU blit, one readback, no upload")
	{
		auto r = req_of(RW, RH, XRT_CONV_FMT_RGBA8_UNORM, CONV_IMAGE_USAGE_INPUT);
		xrt_conv_image slot{};
		REQUIRE(tr_alloc(&rig.tr, &r, &slot));
		REQUIRE(slot.memory_kind == XRT_CONV_MEM_CPU);
		const vk_conv_gpu_stats a = rig.stats();
		REQUIRE(submit(slot) == XRT_SUCCESS);
		const vk_conv_gpu_stats b = rig.stats();
		CHECK(b.imports - a.imports == 1u);
		CHECK(b.gpu_copies - a.gpu_copies == 1u);
		CHECK(b.host_reads - a.host_reads == 1u);
		CHECK(b.host_writes - a.host_writes == 0u);
		std::vector<uint8_t> got((const uint8_t *)slot.cpu_ptr, (const uint8_t *)slot.cpu_ptr + want.size());
		CHECK(bytes_off(got, want) == 0u);
		tr_free(&rig.tr, &slot);
	}
	SECTION("GPU slot (a provider that imports IOSurfaces): the region copied GPU -> GPU, no host memory")
	{
		vk_conv_gpu_set_provider_kinds(rig.x.g, XRT_CONV_MEM_IOSURFACE);
		CHECK(vk_conv_gpu_input_memory_kind(rig.x.g, XRT_CONV_MEM_IOSURFACE, 0) == XRT_CONV_MEM_IOSURFACE);
		auto r = req_of(RW, RH, XRT_CONV_FMT_RGBA8_UNORM, CONV_IMAGE_USAGE_INPUT, 1, XRT_CONV_MEM_IOSURFACE);
		xrt_conv_image slot{};
		REQUIRE(rig.x.ops.alloc(rig.x.ops.ud, &r, &slot));
		REQUIRE(slot.memory_kind == XRT_CONV_MEM_IOSURFACE);
		const vk_conv_gpu_stats a = rig.stats();
		REQUIRE(submit(slot) == XRT_SUCCESS);
		const vk_conv_gpu_stats b = rig.stats();
		CHECK(b.imports - a.imports == 1u);
		CHECK(b.gpu_copies - a.gpu_copies == 1u);
		CHECK(b.host_reads - a.host_reads == 0u); // the zero-copy claim: no host buffer touched
		CHECK(b.host_writes - a.host_writes == 0u);
		std::vector<uint8_t> got(want.size());
		REQUIRE(vk_conv_gpu_read(rig.x.g, &slot, got.data(), 0));
		CHECK(bytes_off(got, want) == 0u);
		rig.x.ops.free(rig.x.ops.ud, &slot);
		vk_conv_gpu_set_provider_kinds(rig.x.g, 0);
	}
	SECTION("a region outside the image is refused (the wire is not trusted)")
	{
		auto r = req_of(RW, RH, XRT_CONV_FMT_RGBA8_UNORM, CONV_IMAGE_USAGE_INPUT);
		xrt_conv_image slot{};
		REQUIRE(tr_alloc(&rig.tr, &r, &slot));
		IOSurfaceRef s = make_global_iosurface(16, 8, 'BGRA', pattern(16, 8, 1));
		conv_input_import in{};
		in.kind = CONV_INPUT_IOSURFACE;
		in.handle = (xrt_graphics_buffer_handle_t)s;
		in.sync_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
		in.w = RW;
		in.h = RH;
		in.img_w = W; // a lie: the surface is 16x8
		in.img_h = H;
		in.img_format = XRT_CONV_FMT_BGRA8_UNORM;
		conv_submit_slot cs{};
		cs.image_count = 1;
		cs.images[0] = slot;
		CHECK(rig.cpu_io.import_input_ex(rig.cpu_io.ud, 1, &in, &cs) == XRT_ERROR_OUTPUT_REQUEST_FAILURE);
		tr_free(&rig.tr, &slot);
	}
	CHECK(vk_conv_gpu_image_count(rig.x.g) == 0u);
}

TEST_CASE("transport (macOS): a GPU result exported as a global IOSurface, GPU -> GPU, and imported back byte-exact",
          "[conv_vk_gpu][transport][iosurface]")
{
	TransportRig rig;
	if (!rig.init()) {
		SKIP("no Vulkan device / CPU transport");
	}
	if ((vk_conv_gpu_export_kinds(rig.x.g) & XRT_CONV_MEM_IOSURFACE) == 0) {
		SKIP("VK_EXT_metal_objects / VK_EXT_external_memory_metal not available");
	}
	struct vk_bundle *vk = rig.x.vk.get();
	// The composed views as S5 leaves them: a runtime-only GPU image, 2 views in a row.
	const uint32_t W = 64, H = 16;
	auto r = req_of(W, H, XRT_CONV_FMT_RGBA8_UNORM, CONV_IMAGE_USAGE_RENDER_TARGET, 1, 0, 77);
	r.role = XRT_CONV_OUT_VIEWS;
	xrt_conv_image views{};
	REQUIRE(rig.x.ops.alloc(rig.x.ops.ud, &r, &views));
	const std::vector<uint8_t> px = pattern(W, H, 9);
	REQUIRE(vk_conv_gpu_write(rig.x.g, &views, px.data(), 0));

	const uint64_t sid = 77;
	ipc_server_conv_cpu_io_stream_export(sid, XRT_CONV_MEM_IOSURFACE);
	conv_result res{};
	res.written = XRT_CONV_OUT_VIEWS;
	res.views = views;
	res.views_info.layout = XRT_CONV_VIEWS_ROW;
	res.views_info.view_count = 2;

	const vk_conv_gpu_stats a = rig.stats();
	ipc_server_conv_published pub{};
	REQUIRE(rig.cpu_io.publish_result(rig.cpu_io.ud, sid, &res, &pub) == XRT_SUCCESS);
	const vk_conv_gpu_stats b = rig.stats();
	CHECK(pub.realloc);
	CHECK(pub.width == W);
	CHECK(pub.height == H);
	CHECK(b.gpu_copies - a.gpu_copies == 1u); // the result -> the export image, on the GPU
	CHECK(b.host_reads - a.host_reads == 0u); // no readback (the CPU export reads one back)
	CHECK(b.host_writes - a.host_writes == 0u);

	xrt_graphics_buffer_handle_t h = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	uint32_t ew = 0, eh = 0, ef = 0;
	REQUIRE(rig.cpu_io.export_output(rig.cpu_io.ud, sid, false, &h, &ew, &eh, &ef));
	CHECK(ew == W);
	CHECK(eh == H);
	CHECK(ef == 28u); // DXGI_FORMAT_R8G8B8A8_UNORM, as the CPU export reports it
	IOSurfaceRef surf = (IOSurfaceRef)h;
	REQUIRE(surf != nullptr);
	// Global: a peer process finds it by ID (what the transport carries).
	IOSurfaceRef looked = IOSurfaceLookup(IOSurfaceGetID(surf));
	REQUIRE(looked != nullptr);
	CFRelease(looked);
	CHECK(bytes_off(iosurface_bytes(surf), px) == 0u); // the CPU view of the export

	// The app's side: import the IOSurface into its own VkImage and read it
	// (VkImportMetalIOSurfaceInfoEXT + the MTLTexture memory import).
	Image app;
	REQUIRE(import_iosurface_app(vk, surf, VK_FORMAT_R8G8B8A8_UNORM, app));
	struct vk_cmd_pool pool = {};
	REQUIRE(vk_cmd_pool_init(vk, &pool, 0) == VK_SUCCESS);
	REQUIRE(run_cmd(vk, &pool, [&](VkCommandBuffer cmd) {
		VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vk_cmd_image_barrier_locked(vk, cmd, app.image, 0, VK_ACCESS_TRANSFER_READ_BIT,
		                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, range);
		return true;
	}));
	std::vector<uint8_t> back(px.size());
	REQUIRE(readback(vk, &pool, app, back.data(), back.size()));
	vk_cmd_pool_destroy(vk, &pool);
	CHECK(bytes_off(back, px) == 0u);

	// The next result of the same extent reuses the image (no realloc, same surface).
	const std::vector<uint8_t> px2 = pattern(W, H, 33);
	REQUIRE(vk_conv_gpu_write(rig.x.g, &views, px2.data(), 0));
	ipc_server_conv_published pub2{};
	REQUIRE(rig.cpu_io.publish_result(rig.cpu_io.ud, sid, &res, &pub2) == XRT_SUCCESS);
	CHECK_FALSE(pub2.realloc);
	xrt_graphics_buffer_handle_t h2 = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	REQUIRE(rig.cpu_io.export_output(rig.cpu_io.ud, sid, false, &h2, nullptr, nullptr, nullptr));
	CHECK(h2 == h);
	CHECK(bytes_off(iosurface_bytes(surf), px2) == 0u);

	// A CPU-memory result (a provider that emits views itself) is uploaded into it.
	std::vector<uint8_t> cpu_px = pattern(W, H, 71);
	conv_result cres = res;
	cres.views.memory_kind = XRT_CONV_MEM_CPU;
	cres.views.cpu_ptr = cpu_px.data();
	cres.views.byte_size = cpu_px.size();
	cres.views.row_stride = W * 4;
	cres.views.import_key = 1ull << 63;
	ipc_server_conv_published pub3{};
	REQUIRE(rig.cpu_io.publish_result(rig.cpu_io.ud, sid, &cres, &pub3) == XRT_SUCCESS);
	CHECK(bytes_off(iosurface_bytes(surf), cpu_px) == 0u);

	rig.cpu_io.stream_gone(rig.cpu_io.ud, sid);
	rig.x.ops.free(rig.x.ops.ud, &views);
	CHECK(vk_conv_gpu_image_count(rig.x.g) == 0u); // the export image went with the stream
}
#endif // XRT_GRAPHICS_BUFFER_HANDLE_IS_METAL

#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_FD)
TEST_CASE("transport (Linux): an OPAQUE_FD image in the contract's shape fills the slot, CPU and GPU",
          "[conv_vk_gpu][transport][opaque_fd]")
{
	TransportRig rig;
	if (!rig.init()) {
		SKIP("no Vulkan device / CPU transport");
	}
	if ((vk_conv_gpu_import_kinds(rig.x.g) & XRT_CONV_MEM_VK_OPAQUE_FD) == 0 ||
	    (vk_conv_gpu_export_kinds(rig.x.g) & XRT_CONV_MEM_VK_OPAQUE_FD) == 0) {
		SKIP("VK_KHR_external_memory_fd not available (OPAQUE_FD)");
	}
	// The app's image: the glue's own OPAQUE_FD export has the contract's
	// creation parameters, so it stands in for the app's allocation.
	const uint32_t W = 40, H = 24, X = 4, Y = 2, RW = 32, RH = 16;
	xrt_conv_image app{};
	REQUIRE(vk_conv_gpu_export_alloc(rig.x.g, XRT_CONV_MEM_VK_OPAQUE_FD, XRT_CONV_FMT_RGBA8_UNORM, W, H, 5, &app));
	const std::vector<uint8_t> src = pattern(W, H, 3);
	REQUIRE(vk_conv_gpu_write(rig.x.g, &app, src.data(), 0));
	const std::vector<uint8_t> want = expect_region(src, W, X, Y, RW, RH, false);

	auto submit = [&](const xrt_conv_image &dst) {
		conv_input_import in{};
		in.kind = CONV_INPUT_OPAQUE_FD;
		in.handle = dup((int)app.handle); // what the transport delivers: the table closes it
		in.sync_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
		in.x = X;
		in.y = Y;
		in.w = RW;
		in.h = RH;
		in.img_w = W;
		in.img_h = H;
		in.img_format = XRT_CONV_FMT_RGBA8_UNORM;
		conv_submit_slot slot{};
		slot.image_count = 1;
		slot.images[0] = dst;
		return rig.cpu_io.import_input_ex(rig.cpu_io.ud, 1, &in, &slot);
	};
	{
		auto r = req_of(RW, RH, XRT_CONV_FMT_RGBA8_UNORM, CONV_IMAGE_USAGE_INPUT);
		xrt_conv_image slot{};
		REQUIRE(tr_alloc(&rig.tr, &r, &slot));
		REQUIRE(submit(slot) == XRT_SUCCESS);
		std::vector<uint8_t> got((const uint8_t *)slot.cpu_ptr, (const uint8_t *)slot.cpu_ptr + want.size());
		CHECK(bytes_off(got, want) == 0u);
		tr_free(&rig.tr, &slot);
	}
	{
		vk_conv_gpu_set_provider_kinds(rig.x.g, XRT_CONV_MEM_VK_OPAQUE_FD);
		auto r = req_of(RW, RH, XRT_CONV_FMT_RGBA8_UNORM, CONV_IMAGE_USAGE_INPUT, 1, XRT_CONV_MEM_VK_OPAQUE_FD);
		xrt_conv_image slot{};
		REQUIRE(rig.x.ops.alloc(rig.x.ops.ud, &r, &slot));
		const vk_conv_gpu_stats a = rig.stats();
		REQUIRE(submit(slot) == XRT_SUCCESS);
		const vk_conv_gpu_stats b = rig.stats();
		CHECK(b.host_reads - a.host_reads == 0u);
		CHECK(b.host_writes - a.host_writes == 0u);
		std::vector<uint8_t> got(want.size());
		REQUIRE(vk_conv_gpu_read(rig.x.g, &slot, got.data(), 0));
		CHECK(bytes_off(got, want) == 0u);
		rig.x.ops.free(rig.x.ops.ud, &slot);
		vk_conv_gpu_set_provider_kinds(rig.x.g, 0);
	}
	t_export_free(rig.x.g, &app);
	CHECK(vk_conv_gpu_image_count(rig.x.g) == 0u);
}

TEST_CASE("transport (Linux): an OPAQUE_FD result export re-imports with the contract's parameters",
          "[conv_vk_gpu][transport][opaque_fd]")
{
	TransportRig rig;
	if (!rig.init()) {
		SKIP("no Vulkan device / CPU transport");
	}
	if ((vk_conv_gpu_export_kinds(rig.x.g) & XRT_CONV_MEM_VK_OPAQUE_FD) == 0) {
		SKIP("VK_KHR_external_memory_fd not available (OPAQUE_FD)");
	}
	CHECK((ipc_server_conv_cpu_io_gpu_image_kinds(true) & (1ull << IPC_SERVER_CONV_IMAGE_KIND_DMABUF)) == 0u);
	const uint32_t W = 64, H = 16;
	auto r = req_of(W, H, XRT_CONV_FMT_RGBA8_UNORM, CONV_IMAGE_USAGE_RENDER_TARGET, 1, 0, 88);
	r.role = XRT_CONV_OUT_VIEWS;
	xrt_conv_image views{};
	REQUIRE(rig.x.ops.alloc(rig.x.ops.ud, &r, &views));
	const std::vector<uint8_t> px = pattern(W, H, 17);
	REQUIRE(vk_conv_gpu_write(rig.x.g, &views, px.data(), 0));
	ipc_server_conv_cpu_io_stream_export(88, XRT_CONV_MEM_VK_OPAQUE_FD);
	conv_result res{};
	res.written = XRT_CONV_OUT_VIEWS;
	res.views = views;
	res.views_info.layout = XRT_CONV_VIEWS_ROW;
	res.views_info.view_count = 2;
	const vk_conv_gpu_stats a = rig.stats();
	ipc_server_conv_published pub{};
	REQUIRE(rig.cpu_io.publish_result(rig.cpu_io.ud, 88, &res, &pub) == XRT_SUCCESS);
	const vk_conv_gpu_stats b = rig.stats();
	CHECK(b.host_reads - a.host_reads == 0u);
	xrt_graphics_buffer_handle_t h = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	REQUIRE(rig.cpu_io.export_output(rig.cpu_io.ud, 88, false, &h, nullptr, nullptr, nullptr));
	REQUIRE(h >= 0);
	// The app's side: the export fd into a slot through the import path (the
	// contract's creation parameters on both ends), compared byte-exact.
	auto sr = req_of(W, H, XRT_CONV_FMT_RGBA8_UNORM, CONV_IMAGE_USAGE_INPUT);
	xrt_conv_image slot{};
	REQUIRE(tr_alloc(&rig.tr, &sr, &slot));
	conv_input_import in{};
	in.kind = CONV_INPUT_OPAQUE_FD;
	in.handle = dup(h);
	in.sync_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	in.w = W;
	in.h = H;
	in.img_w = W;
	in.img_h = H;
	in.img_format = XRT_CONV_FMT_RGBA8_UNORM;
	conv_submit_slot cs{};
	cs.image_count = 1;
	cs.images[0] = slot;
	REQUIRE(rig.cpu_io.import_input_ex(rig.cpu_io.ud, 1, &in, &cs) == XRT_SUCCESS);
	std::vector<uint8_t> got((const uint8_t *)slot.cpu_ptr, (const uint8_t *)slot.cpu_ptr + px.size());
	CHECK(bytes_off(got, px) == 0u);
	tr_free(&rig.tr, &slot);
	rig.cpu_io.stream_gone(rig.cpu_io.ud, 88);
	rig.x.ops.free(rig.x.ops.ud, &views);
	CHECK(vk_conv_gpu_image_count(rig.x.g) == 0u);
}
#endif // XRT_GRAPHICS_BUFFER_HANDLE_IS_FD

#if defined(XRT_OS_LINUX_DESKTOP)
TEST_CASE("transport (Linux): a single-plane dma-buf fills the slot", "[conv_vk_gpu][transport][dmabuf]")
{
	TransportRig rig;
	if (!rig.init()) {
		SKIP("no Vulkan device / CPU transport");
	}
	if ((vk_conv_gpu_import_kinds(rig.x.g) & XRT_CONV_MEM_DMABUF) == 0) {
		SKIP("VK_EXT_external_memory_dma_buf + VK_EXT_image_drm_format_modifier not available");
	}
	// The app's dma-buf: a glue DMABUF image (single-plane modifier, offset 0).
	vk_conv_gpu_set_provider_kinds(rig.x.g, XRT_CONV_MEM_DMABUF);
	const uint32_t W = 40, H = 24;
	auto ar = req_of(W, H, XRT_CONV_FMT_RGBA8_UNORM, CONV_IMAGE_USAGE_INPUT, 1, XRT_CONV_MEM_DMABUF, 6);
	xrt_conv_image app{};
	if (!rig.x.ops.alloc(rig.x.ops.ud, &ar, &app) || app.memory_kind != XRT_CONV_MEM_DMABUF) {
		vk_conv_gpu_set_provider_kinds(rig.x.g, 0);
		SKIP("no exportable single-plane dma-buf modifier for RGBA8");
	}
	vk_conv_gpu_set_provider_kinds(rig.x.g, 0);
	const std::vector<uint8_t> src = pattern(W, H, 11);
	REQUIRE(vk_conv_gpu_write(rig.x.g, &app, src.data(), 0));
	auto sr = req_of(W, H, XRT_CONV_FMT_RGBA8_UNORM, CONV_IMAGE_USAGE_INPUT);
	xrt_conv_image slot{};
	REQUIRE(tr_alloc(&rig.tr, &sr, &slot));
	conv_input_import in{};
	in.kind = CONV_INPUT_DMABUF;
	in.handle = dup((int)app.handle);
	in.sync_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	in.w = W;
	in.h = H;
	in.img_w = W;
	in.img_h = H;
	in.img_format = XRT_CONV_FMT_RGBA8_UNORM;
	in.cpu_row_pitch = app.row_stride;
	in.drm_modifier = app.drm_modifier;
	conv_submit_slot cs{};
	cs.image_count = 1;
	cs.images[0] = slot;
	REQUIRE(rig.cpu_io.import_input_ex(rig.cpu_io.ud, 1, &in, &cs) == XRT_SUCCESS);
	std::vector<uint8_t> got((const uint8_t *)slot.cpu_ptr, (const uint8_t *)slot.cpu_ptr + src.size());
	CHECK(bytes_off(got, src) == 0u);
	tr_free(&rig.tr, &slot);
	rig.x.ops.free(rig.x.ops.ud, &app);
}
#endif // XRT_OS_LINUX_DESKTOP


/*
 *
 * Step C: lifted weave rects (vk_conv_weave + the POSIX bridge).
 *
 */

namespace {

//! The POSIX service's composition (ipc_server_conv_vk.c) around a depth-only provider.
struct WeaveRig
{
	Glue x;
	conv_fake_provider f{};
	Tramp tr;
	conv_core *core = nullptr;
	ipc_server_conv *sc = nullptr;
	ipc_server_conv_io_ops cpu_io{};
	bool ok = false;

	bool
	init(uint32_t extra_import_kinds = 0)
	{
		if (!ipc_server_conv_cpu_io_get(&cpu_io) || !make_glue(x)) {
			return false;
		}
		conv_fake_init(&f, "depth-only");
		conv_fake_cap *c =
		    conv_fake_add_cap(&f, 2, XRT_CONV_TASK_DEPTH_VIDEO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_DEPTH, 0);
		c->state = XRT_CONV_STATE_READY;
		c->cap.depth_kind = XRT_CONV_DEPTH_METRIC;
		c->cap.import_memory_kinds |= extra_import_kinds;
		tr.gpu = x.ops;
		conv_core_config cfg{};
		cfg.alloc.alloc = tr_alloc;
		cfg.alloc.free = tr_free;
		cfg.alloc.upload = tr_upload;
		cfg.alloc.ud = &tr;
		cfg.prewarm_on = CONV_PREWARM_FIRST_CREATE;
		cfg.prewarm_task_mask = 1ull << 63;
		cfg.rear_cap_m = -1.0f;
		cfg.tick_ms = 10;
		REQUIRE(conv_core_create(&cfg, &core) == XRT_SUCCESS);
		REQUIRE(ipc_server_conv_create(core, &sc) == XRT_SUCCESS);
		ipc_server_conv_set_io(sc, &cpu_io);
		ipc_server_conv_cpu_gpu copies{hook_write, hook_read, x.g};
		ipc_server_conv_cpu_io_set_gpu(&copies);
		ipc_server_conv_gpu gpu{};
		gpu.alloc = x.ops;
		gpu.viewsyn = x.vs;
		gpu.readback = x.rb;
		gpu.io = cpu_io;
		ipc_server_conv_install_gpu(sc, &gpu);
		conv_provider_desc d{};
		d.id = f.id;
		d.probe_order = 100;
		d.enabled = true;
		d.iface = &f.iface;
		d.inst = conv_fake_inst(&f);
		REQUIRE(conv_core_add_provider(core, &d) >= 0);
		ok = true;
		return true;
	}

	~WeaveRig()
	{
		if (!ok) {
			return;
		}
		ipc_server_conv_set_weave_rects(sc, false);
		ipc_server_conv_install_gpu(sc, nullptr);
		ipc_server_conv_cpu_io_set_gpu(nullptr);
		ipc_server_conv_destroy(&sc);
		conv_core_destroy(&core);
		conv_fake_fini(&f);
	}
};

bool
weave_resolve(void *ud, uint64_t owner, uint64_t wire_id, uint64_t *out_core_id, uint32_t *out_mode)
{
	return ipc_server_conv_resolve_stream(static_cast<ipc_server_conv *>(ud), owner, wire_id, out_core_id,
	                                      out_mode);
}

void
weave_match(void *ud, uint64_t owner, uint64_t wire_id, uint32_t views)
{
	ipc_server_conv_v4_match_weave(static_cast<ipc_server_conv *>(ud), owner, wire_id, views);
}

} // namespace

TEST_CASE("weave rects under the CPU route: spec-3 SBS streams, tagged ids through the bridge, per-DP eyes",
          "[conv_vk_gpu][weave_rects]")
{
	WeaveRig rig;
	if (!rig.init()) {
		SKIP("no Vulkan device / CPU transport available");
	}
	ipc_server_conv *sc = rig.sc;
	// cpu_only: spec 3 is off until a weave engine can lift rects.
	CHECK_FALSE(ipc_server_conv_routes_create(sc, XRT_DP_LIFT_MODE_SBS, 0));
	CHECK_FALSE(ipc_server_conv_routes_properties(sc));
	ipc_server_conv_set_weave_rects(sc, true);
	CHECK(ipc_server_conv_routes_create(sc, XRT_DP_LIFT_MODE_SBS, 0));
	CHECK_FALSE(ipc_server_conv_routes_create(sc, XRT_DP_LIFT_MODE_DEPTH, 0));
	CHECK_FALSE(ipc_server_conv_routes_create(sc, XRT_DP_LIFT_MODE_GAUSSIANS, 0));
	CHECK(ipc_server_conv_routes_properties(sc));
	xrt_dp_lift_caps caps{};
	ipc_server_conv_lift_properties(sc, &caps);
	CHECK((caps.modes & XRT_DP_LIFT_MODE_SBS) != 0u);
	CHECK((caps.modes & (XRT_DP_LIFT_MODE_DEPTH | XRT_DP_LIFT_MODE_GAUSSIANS)) == 0u);

	// The weave client's spec-3 SBS stream: a tagged wire id the bridge resolves
	// for its owner only.
	uint64_t wire = 0;
	REQUIRE(ipc_server_conv_lift_stream_create(sc, 1, XRT_DP_LIFT_MODE_SBS, 0, 1.0f, 0, 0, &wire) == XRT_SUCCESS);
	CHECK((wire & IPC_SERVER_CONV_STREAM_TAG) != 0u);
	uint64_t core_id = 0;
	uint32_t mode = 0;
	CHECK(weave_resolve(sc, 1, wire, &core_id, &mode));
	CHECK(core_id != 0u);
	CHECK(mode == XRT_DP_LIFT_MODE_SBS);
	CHECK_FALSE(weave_resolve(sc, 2, wire, &core_id, &mode));
	CHECK_FALSE(weave_resolve(sc, 1, wire & ~IPC_SERVER_CONV_STREAM_TAG, &core_id, &mode));

	// The per-DP eyes sink: published under the DP's key, read back, retired.
	xrt_eye_positions e{};
	e.valid = true;
	e.is_tracking = true;
	e.count = 2;
	e.eyes[0].x = -0.031f;
	e.eyes[0].z = 0.6f;
	e.eyes[1].x = 0.031f;
	e.eyes[1].z = 0.6f;
	const uint64_t dp_a = 0x1000, dp_b = 0x2000;
	ipc_server_conv_eyes_publish(sc, dp_a, &e);
	e.eyes[0].x = -0.5f;
	ipc_server_conv_eyes_publish(sc, dp_b, &e);
	conv_eyes ce{};
	REQUIRE(conv_core_eyes_read(rig.core, dp_a, now_ns(), &ce));
	CHECK(ce.valid);
	CHECK(ce.count == 2u);
	CHECK(ce.xyz[0][0] == -0.031f);
	CHECK(ce.xyz[1][2] == 0.6f);
	REQUIRE(conv_core_eyes_read(rig.core, dp_b, now_ns(), &ce));
	CHECK(ce.xyz[0][0] == -0.5f); // per DP, never another screen's
	ipc_server_conv_eyes_retire(sc, dp_a);
	CHECK_FALSE(conv_core_eyes_read(rig.core, dp_a, now_ns(), &ce));
	CHECK(conv_core_eyes_read(rig.core, dp_b, now_ns(), &ce));

	CHECK(ipc_server_conv_lift_stream_destroy(sc, 1, wire) == XRT_SUCCESS);
	CHECK_FALSE(weave_resolve(sc, 1, wire, &core_id, &mode));
	ipc_server_conv_set_weave_rects(sc, false);
	CHECK_FALSE(ipc_server_conv_routes_create(sc, XRT_DP_LIFT_MODE_SBS, 0));
}

TEST_CASE("vk_conv_weave: a lifted batch rect -- flat, capped snapshot, woven result, letterbox crop",
          "[conv_vk_gpu][weave_rects]")
{
	WeaveRig rig;
	if (!rig.init()) {
		SKIP("no Vulkan device / CPU transport available");
	}
	struct vk_bundle *vk = rig.x.vk.get();
	ipc_server_conv *sc = rig.sc;
	ipc_server_conv_set_weave_rects(sc, true);
	uint64_t wire = 0;
	REQUIRE(ipc_server_conv_lift_stream_create(sc, 1, XRT_DP_LIFT_MODE_SBS, 0, 1.0f, 0, 0, &wire) == XRT_SUCCESS);
	uint64_t core_id = 0;
	uint32_t mode = 0;
	REQUIRE(weave_resolve(sc, 1, wire, &core_id, &mode));

	vk_conv_weave *w = nullptr;
	REQUIRE(vk_conv_weave_create(vk, rig.x.g, &w));
	vk_conv_weave_bridge b{};
	b.core = rig.core;
	b.resolve = weave_resolve;
	b.match_weave = weave_match;
	b.fit_rect = ipc_server_conv_submit_rect;
	b.ud = sc;
	vk_conv_weave_set_bridge(w, &b);
	CHECK(vk_conv_weave_stream_mode(w, 1, wire) == XRT_DP_LIFT_MODE_SBS);
	CHECK(vk_conv_weave_stream_mode(w, 2, wire) == 0u);
	vk_conv_weave_client *c = vk_conv_weave_client_create(w, vk);
	REQUIRE(c != nullptr);

	// The window-sized input (BGRA, the Linux / macOS weave input contract):
	// red outside the rect; inside a 2400x1000 rect, a film with 100-row black
	// bars top and bottom.
	const uint32_t W = 2560, H = 1200;
	const int32_t RX = 80, RY = 100;
	const uint32_t RW = 2400, RH = 1000, BAR = 100;
	std::vector<uint8_t> src((size_t)W * H * 4);
	for (uint32_t y = 0; y < H; y++) {
		for (uint32_t x = 0; x < W; x++) {
			uint8_t *p = &src[((size_t)y * W + x) * 4];
			const bool in_rect = (int32_t)x >= RX && x < RX + RW && (int32_t)y >= RY && y < RY + RH;
			const uint32_t ry = y - RY;
			if (!in_rect) {
				p[0] = 0, p[1] = 0, p[2] = 255; // B G R: red
			} else if (ry < BAR || ry >= RH - BAR) {
				p[0] = 0, p[1] = 0, p[2] = 0; // the bars
			} else {
				p[0] = (uint8_t)(40 + x % 200), p[1] = 128, p[2] = (uint8_t)(40 + y % 200);
			}
			p[3] = 255;
		}
	}
	Image in_img, sbs;
	REQUIRE(make_image(vk, in_img, W, H, 1, VK_FORMAT_B8G8R8A8_UNORM));
	REQUIRE(make_image(vk, sbs, 2 * W, H, 1, VK_FORMAT_R8G8B8A8_UNORM));
	vk_cmd_pool pool{};
	REQUIRE(vk_cmd_pool_init(vk, &pool, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) == VK_SUCCESS);
	REQUIRE(upload(vk, &pool, in_img, src.data(), src.size()));
	std::vector<uint8_t> zeros((size_t)2 * W * H * 4, 0);
	REQUIRE(upload(vk, &pool, sbs, zeros.data(), zeros.size()));

	xrt_lift_weave_rect binding{};
	binding.stream_id = wire;
	binding.rect_index = 0;
	vk_conv_weave_rect r{};
	r.binding = &binding;
	r.owner = 1;
	r.src = {in_img.image, VK_IMAGE_LAYOUT_GENERAL, VK_FORMAT_B8G8R8A8_UNORM, W, H};
	r.x = RX;
	r.y = RY;
	r.w = RW;
	r.h = RH;
	r.dst = {sbs.image, VK_IMAGE_LAYOUT_GENERAL, VK_FORMAT_R8G8B8A8_UNORM, 2 * W, H};
	r.views = 2;
	r.tile_columns = 2;
	r.tile_w = W;
	r.tile_h = H;
	r.batch = true;
	r.frame.valid = true;
	r.frame.width_m = 0.30f;
	r.frame.height_m = 0.125f;

	auto weave = [&]() {
		vk_conv_weave_begin(w, c);
		const bool rec =
		    run_cmd(vk, &pool, [&](VkCommandBuffer cmd) { return vk_conv_weave_record(w, c, cmd, &r); });
		vk_conv_weave_complete(w, c); // the "fence" signalled: run_cmd waited
		return rec;
	};
	std::vector<uint8_t> out((size_t)2 * W * H * 4);
	auto px = [&](uint32_t x, uint32_t y) { return &out[((size_t)y * 2 * W + x) * 4]; };
	auto src_rgba_eq = [&](uint32_t sx, uint32_t sy, const uint8_t *d) {
		const uint8_t *s = &src[((size_t)sy * W + sx) * 4];
		return d[0] == s[2] && d[1] == s[1] && d[2] == s[0] && d[3] == s[3];
	};

	// 1. Flat until the first result; the snapshot is capped to the 1920 long edge.
	REQUIRE(weave());
	vk_conv_weave_stats st{};
	REQUIRE(vk_conv_weave_client_stats(c, wire, &st));
	CHECK(st.active_w == RW);
	CHECK(st.active_h == RH);
	CHECK(st.snap_w == 1920u);
	CHECK(st.snap_h == 800u);
	CHECK(st.committed == 1u);
	CHECK_FALSE(st.woven_result);
	REQUIRE(readback(vk, &pool, sbs, out.data(), out.size()));
	CHECK(src_rgba_eq(580, 400, px(580, 400)));     // tile 0: the 2D frame, 1:1
	CHECK(src_rgba_eq(580, 400, px(W + 580, 400))); // tile 1: the same pixels (flat)
	CHECK(px(10, 10)[3] == 0);                      // outside the rect: untouched
	CHECK(px(W + RX + RW + 10, RY + 10)[3] == 0);

	// 2. The conversion lands; the next weave samples it into the rect.
	conv_stream_stats ss{};
	REQUIRE(wait_until(
	    [&] { return conv_core_stream_get_stats(rig.core, core_id, &ss) == XRT_SUCCESS && ss.converted >= 1; },
	    5000));
	REQUIRE(weave());
	REQUIRE(vk_conv_weave_client_stats(c, wire, &st));
	CHECK(st.woven_result);
	CHECK(st.result_views == 2u);
	CHECK(st.committed == 2u);
	REQUIRE(readback(vk, &pool, sbs, out.data(), out.size()));
	CHECK(px(580, 400)[3] == 255); // the result, opaque, inside the rect in both tiles
	CHECK(px(W + 580, 400)[3] == 255);
	CHECK(px(10, 10)[3] == 0); // still nothing outside it
	CHECK(px(W + RX + RW + 10, RY + 10)[3] == 0);

	// 3. The letterbox crop settles: the snapshot is the active area only, capped.
	bool cropped = false;
	for (int i = 0; i < 120 && !cropped; i++) {
		REQUIRE(weave());
		REQUIRE(vk_conv_weave_client_stats(c, wire, &st));
		cropped = st.active_h == RH - 2 * BAR;
	}
	REQUIRE(cropped);
	CHECK(st.active_w == RW);
	CHECK(st.snap_w == 1920u);
	CHECK(st.snap_h == 640u);
	// Once a result of the cropped input lands, the bars are the 2D input, flat.
	const uint64_t done = ss.converted;
	REQUIRE(wait_until(
	    [&] {
		    return conv_core_stream_get_stats(rig.core, core_id, &ss) == XRT_SUCCESS && ss.converted > done + 1;
	    },
	    5000));
	REQUIRE(weave());
	REQUIRE(readback(vk, &pool, sbs, out.data(), out.size()));
	CHECK(src_rgba_eq(580, RY + 50, px(580, RY + 50)));
	CHECK(src_rgba_eq(580, RY + 50, px(W + 580, RY + 50)));

	vk_conv_weave_client_destroy(w, c);
	vk_conv_weave_destroy(&w);
	vk_cmd_pool_destroy(vk, &pool);
	CHECK(ipc_server_conv_lift_stream_destroy(sc, 1, wire) == XRT_SUCCESS);
}

TEST_CASE("vk_conv_weave: the snapshot goes GPU -> GPU when the binding takes a GPU kind, host path otherwise",
          "[conv_vk_gpu][weave_rects][weave_gpu_snap]")
{
	// Step E: a composed binding whose depth stage imports a GPU kind gets a
	// GPU slot; the weave's snapshot then never touches host memory. Same
	// pixels as the host path (an uncapped, uncropped rect: the snapshot is
	// the rect's texels, BGRA -> RGBA).
	for (int arm = 0; arm < 2; arm++) {
		const bool gpu = arm == 1;
		DYNAMIC_SECTION((gpu ? "GPU slot" : "CPU slot (host path)"))
		{
			Glue probe;
			if (!make_glue(probe)) {
				SKIP("no Vulkan device available");
			}
			vk_conv_gpu_caps pc{};
			vk_conv_gpu_get_caps(probe.g, &pc);
			const uint32_t gk = pc.memory_kinds & ~XRT_CONV_MEM_CPU;
			if (gpu && gk == 0) {
				SKIP("no exportable GPU memory kind on this device");
			}
			WeaveRig rig;
			if (!rig.init(gpu ? gk : 0)) {
				SKIP("no Vulkan device / CPU transport available");
			}
			rig.f.caps[0].convert_ms = 0;
			// The service's policy hook: providers may take these kinds (in process).
			vk_conv_gpu_set_provider_kinds(rig.x.g, gpu ? gk : 0);
			struct vk_bundle *vk = rig.x.vk.get();
			ipc_server_conv *sc = rig.sc;
			ipc_server_conv_set_weave_rects(sc, true);
			uint64_t wire = 0;
			REQUIRE(ipc_server_conv_lift_stream_create(sc, 1, XRT_DP_LIFT_MODE_SBS, 0, 1.0f, 0, 0, &wire) ==
			        XRT_SUCCESS);
			uint64_t core_id = 0;
			uint32_t mode = 0;
			REQUIRE(weave_resolve(sc, 1, wire, &core_id, &mode));
			uint32_t im = 0, is = 0;
			REQUIRE(conv_core_stream_input_kinds(rig.core, core_id, &im, &is));
			CHECK((vk_conv_gpu_input_memory_kind(rig.x.g, im, is) != XRT_CONV_MEM_CPU) == gpu);

			vk_conv_weave *w = nullptr;
			REQUIRE(vk_conv_weave_create(vk, rig.x.g, &w));
			vk_conv_weave_bridge b{};
			b.core = rig.core;
			b.resolve = weave_resolve;
			b.match_weave = weave_match;
			b.fit_rect = ipc_server_conv_submit_rect;
			b.ud = sc;
			vk_conv_weave_set_bridge(w, &b);
			vk_conv_weave_client *c = vk_conv_weave_client_create(w, vk);
			REQUIRE(c != nullptr);

			const uint32_t W = 256, H = 128;
			const int32_t RX = 40, RY = 20;
			const uint32_t RW = 160, RH = 80;
			const std::vector<uint8_t> src = pattern(W, H, 21); // read as B,G,R,A
			Image in_img, sbs;
			REQUIRE(make_image(vk, in_img, W, H, 1, VK_FORMAT_B8G8R8A8_UNORM));
			REQUIRE(make_image(vk, sbs, 2 * W, H, 1, VK_FORMAT_R8G8B8A8_UNORM));
			vk_cmd_pool pool{};
			REQUIRE(vk_cmd_pool_init(vk, &pool, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) ==
			        VK_SUCCESS);
			REQUIRE(upload(vk, &pool, in_img, src.data(), src.size()));
			std::vector<uint8_t> zeros((size_t)2 * W * H * 4, 0);
			REQUIRE(upload(vk, &pool, sbs, zeros.data(), zeros.size()));
			xrt_lift_weave_rect binding{};
			binding.stream_id = wire;
			vk_conv_weave_rect r{};
			r.binding = &binding;
			r.owner = 1;
			r.src = {in_img.image, VK_IMAGE_LAYOUT_GENERAL, VK_FORMAT_B8G8R8A8_UNORM, W, H};
			r.x = RX;
			r.y = RY;
			r.w = RW;
			r.h = RH;
			r.dst = {sbs.image, VK_IMAGE_LAYOUT_GENERAL, VK_FORMAT_R8G8B8A8_UNORM, 2 * W, H};
			r.views = 2;
			r.tile_columns = 2;
			r.tile_w = W;
			r.tile_h = H;
			r.batch = true;
			r.frame.valid = true;
			r.frame.width_m = 0.30f;
			r.frame.height_m = 0.15f;

			vk_conv_gpu_stats a{}, z{};
			vk_conv_gpu_get_stats(rig.x.g, &a);
			vk_conv_weave_begin(w, c);
			REQUIRE(run_cmd(vk, &pool,
			                [&](VkCommandBuffer cmd) { return vk_conv_weave_record(w, c, cmd, &r); }));
			vk_conv_weave_complete(w, c);
			vk_conv_gpu_get_stats(rig.x.g, &z);
			vk_conv_weave_stats st{};
			REQUIRE(vk_conv_weave_client_stats(c, wire, &st));
			CHECK(st.committed == 1u);
			CHECK(st.snap_w == RW);
			CHECK(st.snap_h == RH);
			if (gpu) {
				CHECK(z.gpu_copies - a.gpu_copies == 1u);   // the chain's image -> the GPU slot
				CHECK(z.host_writes - a.host_writes == 0u); // no host buffer, no upload
				CHECK(z.host_reads - a.host_reads == 0u);
			} else {
				CHECK(z.gpu_copies - a.gpu_copies == 0u); // host buffer -> the CPU slot (a memcpy)
			}

			// What the provider was handed: the snapshot, byte-exact on both paths.
			REQUIRE(wait_until(
			    [&] {
				    os_mutex_lock(&rig.f.mtx);
				    const bool seen = rig.f.converts >= 1;
				    os_mutex_unlock(&rig.f.mtx);
				    return seen;
			    },
			    5000));
			os_mutex_lock(&rig.f.mtx);
			const xrt_conv_image in = rig.f.last_input;
			os_mutex_unlock(&rig.f.mtx);
			const std::vector<uint8_t> want =
			    expect_region(src, W, (uint32_t)RX, (uint32_t)RY, RW, RH, true);
			std::vector<uint8_t> got(want.size());
			if (gpu) {
				REQUIRE(in.memory_kind != XRT_CONV_MEM_CPU);
				REQUIRE(vk_conv_gpu_read(rig.x.g, &in, got.data(), 0));
			} else {
				REQUIRE(in.memory_kind == XRT_CONV_MEM_CPU);
				REQUIRE(in.cpu_ptr != nullptr);
				memcpy(got.data(), in.cpu_ptr, got.size());
			}
			CHECK(bytes_off(got, want) == 0u);

			vk_conv_weave_client_destroy(w, c);
			vk_conv_weave_destroy(&w);
			vk_cmd_pool_destroy(vk, &pool);
			CHECK(ipc_server_conv_lift_stream_destroy(sc, 1, wire) == XRT_SUCCESS);
			vk_conv_gpu_set_provider_kinds(rig.x.g, 0);
		}
	}
}

TEST_CASE("vk_conv_weave: a stream destroyed with a weave in flight keeps its images until the frame completes",
          "[conv_vk_gpu][weave_rects]")
{
	// Stage B on Linux completes a weave at the NEXT submit (weave_wait_prev_frame);
	// between the two the client may destroy the stream. The helper's pins on the
	// woven result keep the core from reaping the stream (its images are the
	// glue's) until vk_conv_weave_complete, which the engine calls only after the
	// weave's fence signalled.
	WeaveRig rig;
	if (!rig.init()) {
		SKIP("no Vulkan device / CPU transport available");
	}
	struct vk_bundle *vk = rig.x.vk.get();
	ipc_server_conv *sc = rig.sc;
	ipc_server_conv_set_weave_rects(sc, true);
	uint64_t wire = 0;
	REQUIRE(ipc_server_conv_lift_stream_create(sc, 1, XRT_DP_LIFT_MODE_SBS, 0, 1.0f, 0, 0, &wire) == XRT_SUCCESS);
	uint64_t core_id = 0;
	uint32_t mode = 0;
	REQUIRE(weave_resolve(sc, 1, wire, &core_id, &mode));
	vk_conv_weave *w = nullptr;
	REQUIRE(vk_conv_weave_create(vk, rig.x.g, &w));
	vk_conv_weave_bridge b{};
	b.core = rig.core;
	b.resolve = weave_resolve;
	b.match_weave = weave_match;
	b.fit_rect = ipc_server_conv_submit_rect;
	b.ud = sc;
	vk_conv_weave_set_bridge(w, &b);
	vk_conv_weave_client *c = vk_conv_weave_client_create(w, vk);
	REQUIRE(c != nullptr);

	const uint32_t W = 640, H = 360;
	std::vector<uint8_t> src((size_t)W * H * 4, 0x70);
	Image in_img, sbs;
	REQUIRE(make_image(vk, in_img, W, H, 1, VK_FORMAT_B8G8R8A8_UNORM));
	REQUIRE(make_image(vk, sbs, 2 * W, H, 1, VK_FORMAT_R8G8B8A8_UNORM));
	vk_cmd_pool pool{};
	REQUIRE(vk_cmd_pool_init(vk, &pool, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) == VK_SUCCESS);
	REQUIRE(upload(vk, &pool, in_img, src.data(), src.size()));
	std::vector<uint8_t> zeros((size_t)2 * W * H * 4, 0);
	REQUIRE(upload(vk, &pool, sbs, zeros.data(), zeros.size()));

	xrt_lift_weave_rect binding{};
	binding.stream_id = wire;
	vk_conv_weave_rect r{};
	r.binding = &binding;
	r.owner = 1;
	r.src = {in_img.image, VK_IMAGE_LAYOUT_GENERAL, VK_FORMAT_B8G8R8A8_UNORM, W, H};
	r.x = 64;
	r.y = 32;
	r.w = 320;
	r.h = 180;
	r.dst = {sbs.image, VK_IMAGE_LAYOUT_GENERAL, VK_FORMAT_R8G8B8A8_UNORM, 2 * W, H};
	r.views = 2;
	r.tile_columns = 2;
	r.tile_w = W;
	r.tile_h = H;
	r.batch = true;
	r.frame.valid = false; // no window metrics: the panel's size, fitted to the rect (fit_rect)
	r.frame.width_m = 0.16f;
	r.frame.height_m = 0.09f;
	auto record = [&]() {
		vk_conv_weave_begin(w, c);
		return run_cmd(vk, &pool, [&](VkCommandBuffer cmd) { return vk_conv_weave_record(w, c, cmd, &r); });
	};

	// Until a result is woven (it is then pinned by the frame).
	vk_conv_weave_stats st{};
	bool woven = false;
	for (int i = 0; i < 200 && !woven; i++) {
		REQUIRE(record());
		REQUIRE(vk_conv_weave_client_stats(c, wire, &st));
		woven = st.woven_result;
		if (!woven) {
			vk_conv_weave_complete(w, c);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}
	REQUIRE(woven);
	// The frame that wove it is "in flight" as far as the helper knows: no complete yet.
	// Let the conversion of the snapshot this frame committed land first, so
	// the count below is not raced by a ring target it allocates (with the
	// asynchronous S5 hand-over on macOS the conversions run back to back).
	conv_stream_stats ss{};
	(void)wait_until(
	    [&] {
		    return conv_core_stream_get_stats(rig.core, core_id, &ss) == XRT_SUCCESS &&
		           ss.converted + ss.dropped + ss.failed >= ss.submitted;
	    },
	    3000);
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	const uint32_t images = vk_conv_gpu_image_count(rig.x.g);
	REQUIRE(images > 0u);

	// The client destroys its stream mid-frame.
	CHECK(ipc_server_conv_lift_stream_destroy(sc, 1, wire) == XRT_SUCCESS);
	CHECK_FALSE(weave_resolve(sc, 1, wire, &core_id, &mode));
	// Give the core's reaper ample ticks (tick_ms = 10): the pinned stream stays.
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	CHECK(vk_conv_gpu_image_count(rig.x.g) == images);

	// The frame's fence signalled: complete drops the pins (and the snapshot of a
	// stream that is gone); only now are the stream's images released.
	vk_conv_weave_complete(w, c);
	CHECK(wait_until([&] { return vk_conv_gpu_image_count(rig.x.g) == 0u; }, 3000));
	CHECK_FALSE(vk_conv_weave_client_stats(c, wire, &st)); // the helper's per-stream state went too

	vk_conv_weave_client_destroy(w, c);
	vk_conv_weave_destroy(&w);
	vk_cmd_pool_destroy(vk, &pool);
}


/*
 *
 * Step C on the macOS Metal weave backend: vk_conv_weave as a host client,
 * comp_multi_weave_metal_lift recording into a headless Metal device. The
 * results stay the Vulkan glue's (MoltenVK) IOSurface exports, sampled in Metal.
 *
 */

#ifdef CONV_TEST_HAVE_METAL_WEAVE

#include "conv_metal_weave_rig.h"

namespace {

//! The planner the Metal engine uses, straight onto the helper (+ the last plan, for the oracle).
struct HostPlanner
{
	vk_conv_weave *w = nullptr;
	vk_conv_weave_client *c = nullptr;
	struct vk_conv_weave_host_plan last
	{
	};
	uint32_t plans = 0;

	static bool
	plan(void *ud, const vk_conv_weave_rect *r, struct vk_conv_weave_host_plan *out)
	{
		auto *hp = static_cast<HostPlanner *>(ud);
		const bool ok = vk_conv_weave_host_plan(hp->w, hp->c, r, out);
		hp->last = *out;
		hp->plans++;
		return ok;
	}

	static void
	done(void *ud, uint64_t owner, uint64_t wire, const struct vk_conv_weave_host_done *d)
	{
		auto *hp = static_cast<HostPlanner *>(ud);
		vk_conv_weave_host_done(hp->w, hp->c, owner, wire, d);
	}

	comp_multi_weave_metal_lift_planner
	get()
	{
		return {plan, done, this};
	}
};

//! Bilinear sample (texel centres at +0.5) of channel @p ch, R G B A order, from an 8-bit IOSurface view.
float
iosurface_bilinear(
    const uint8_t *base, size_t pitch, bool bgra, float x, float y, uint32_t x0, uint32_t w, uint32_t h, int ch)
{
	const int map[4] = {bgra ? 2 : 0, 1, bgra ? 0 : 2, 3};
	const float fx = std::min(std::max(x - 0.5f, 0.0f), (float)(w - 1));
	const float fy = std::min(std::max(y - 0.5f, 0.0f), (float)(h - 1));
	const uint32_t ix = (uint32_t)fx, iy = (uint32_t)fy;
	const uint32_t jx = std::min(ix + 1, w - 1), jy = std::min(iy + 1, h - 1);
	const float ax = fx - (float)ix, ay = fy - (float)iy;
	auto t = [&](uint32_t px, uint32_t py) {
		return (float)base[(size_t)py * pitch + (size_t)(x0 + px) * 4 + map[ch]];
	};
	return (1 - ay) * ((1 - ax) * t(ix, iy) + ax * t(jx, iy)) + ay * ((1 - ax) * t(ix, jy) + ax * t(jx, jy));
}

} // namespace

TEST_CASE("Metal weave backend: a lifted batch rect -- flat, capped snapshot, the IOSurface result sampled, letterbox",
          "[conv_vk_gpu][weave_rects][metal]")
{
	WeaveRig rig;
	if (!rig.init()) {
		SKIP("no Vulkan device / CPU transport available");
	}
	cmw_rig *mr = cmw_rig_create();
	if (mr == nullptr) {
		SKIP("no Metal device");
	}
	ipc_server_conv *sc = rig.sc;
	ipc_server_conv_set_weave_rects(sc, true);
	uint64_t wire = 0;
	REQUIRE(ipc_server_conv_lift_stream_create(sc, 1, XRT_DP_LIFT_MODE_SBS, 0, 1.0f, 0, 0, &wire) == XRT_SUCCESS);
	uint64_t core_id = 0;
	uint32_t mode = 0;
	REQUIRE(weave_resolve(sc, 1, wire, &core_id, &mode));

	vk_conv_weave *w = nullptr;
	REQUIRE(vk_conv_weave_create(rig.x.vk.get(), rig.x.g, &w));
	vk_conv_weave_bridge b{};
	b.core = rig.core;
	b.resolve = weave_resolve;
	b.match_weave = weave_match;
	b.fit_rect = ipc_server_conv_submit_rect;
	b.ud = sc;
	vk_conv_weave_set_bridge(w, &b);
	HostPlanner hp;
	hp.w = w;
	hp.c = vk_conv_weave_client_create(w, nullptr); // a host client
	REQUIRE(hp.c != nullptr);
	comp_multi_weave_metal_lift *ml = comp_multi_weave_metal_lift_create(cmw_rig_device(mr));
	REQUIRE(ml != nullptr);
	const comp_multi_weave_metal_lift_planner pl = hp.get();

	// The engine's input (BGRA): red outside the rect; inside a 2400x1000
	// rect, a film with 100-row black bars top and bottom. The SBS scratch
	// (BGRA, 2W x H) starts transparent.
	const uint32_t W = 2560, H = 1200;
	const int32_t RX = 80, RY = 100;
	const uint32_t RW = 2400, RH = 1000, BAR = 100;
	std::vector<uint8_t> src((size_t)W * H * 4);
	for (uint32_t y = 0; y < H; y++) {
		for (uint32_t x = 0; x < W; x++) {
			uint8_t *p = &src[((size_t)y * W + x) * 4];
			const bool in_rect = (int32_t)x >= RX && x < RX + RW && (int32_t)y >= RY && y < RY + RH;
			const uint32_t ry = y - RY;
			if (!in_rect) {
				p[0] = 0, p[1] = 0, p[2] = 255; // B G R: red
			} else if (ry < BAR || ry >= RH - BAR) {
				p[0] = 0, p[1] = 0, p[2] = 0; // the bars
			} else {
				p[0] = (uint8_t)(40 + x % 200), p[1] = 128, p[2] = (uint8_t)(40 + y % 200);
			}
			p[3] = 255;
		}
	}
	void *in_tex = cmw_rig_texture(mr, W, H, true, src.data());
	void *sbs = cmw_rig_texture(mr, 2 * W, H, true, nullptr);
	REQUIRE(in_tex != nullptr);
	REQUIRE(sbs != nullptr);

	xrt_lift_weave_rect binding{};
	binding.stream_id = wire;
	vk_conv_weave_rect r{};
	r.binding = &binding;
	r.owner = 1;
	r.src = {VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_FORMAT_B8G8R8A8_UNORM, W, H};
	r.x = RX;
	r.y = RY;
	r.w = RW;
	r.h = RH;
	r.dst = {VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_FORMAT_B8G8R8A8_UNORM, 2 * W, H};
	r.views = 2;
	r.tile_columns = 2;
	r.tile_w = W;
	r.tile_h = H;
	r.batch = true;
	r.frame.valid = true;
	r.frame.width_m = 0.30f;
	r.frame.height_m = 0.125f;

	// begin -> record + wait + frame_done -> (optional oracle) -> complete
	std::function<void()> before_complete;
	auto weave = [&]() {
		vk_conv_weave_begin(w, hp.c);
		const int n = cmw_rig_weave(mr, ml, &pl, in_tex, sbs, &r, 1);
		if (before_complete) {
			before_complete();
		}
		vk_conv_weave_complete(w, hp.c);
		return n == 1;
	};
	std::vector<uint8_t> out((size_t)2 * W * H * 4);
	auto px = [&](uint32_t x, uint32_t y) { return &out[((size_t)y * 2 * W + x) * 4]; }; // B G R A
	auto src_eq = [&](uint32_t sx, uint32_t sy, const uint8_t *d) {
		return memcmp(&src[((size_t)sy * W + sx) * 4], d, 4) == 0;
	};

	// 1. Flat until the first result; the snapshot is capped to the 1920 long edge.
	REQUIRE(weave());
	vk_conv_weave_stats st{};
	REQUIRE(vk_conv_weave_client_stats(hp.c, wire, &st));
	CHECK(st.active_w == RW);
	CHECK(st.active_h == RH);
	CHECK(st.snap_w == 1920u);
	CHECK(st.snap_h == 800u);
	CHECK(st.committed == 1u);
	CHECK_FALSE(st.woven_result);
	CHECK(hp.last.lines);
	REQUIRE(cmw_rig_read(mr, sbs, out.data()));
	CHECK(src_eq(580, 400, px(580, 400)));     // tile 0: the 2D frame, 1:1
	CHECK(src_eq(580, 400, px(W + 580, 400))); // tile 1: the same pixels (flat)
	CHECK(src_eq(RX, RY, px(RX, RY)));         // the rect's corners, exact
	CHECK(src_eq(RX + RW - 1, RY + RH - 1, px(W + RX + RW - 1, RY + RH - 1)));
	CHECK(px(10, 10)[3] == 0); // outside the rect: untouched
	CHECK(px(RX - 1, RY + 10)[3] == 0);
	CHECK(px(W + RX + RW, RY + 10)[3] == 0);

	// 2. The conversion lands; the next weave samples the result's IOSurface
	// into the rect. Oracle: the tile pixels equal the IOSurface's views,
	// stretched (read on the CPU while the pin still holds it).
	conv_stream_stats ss{};
	REQUIRE(wait_until(
	    [&] { return conv_core_stream_get_stats(rig.core, core_id, &ss) == XRT_SUCCESS && ss.converted >= 1; },
	    5000));
	struct vk_conv_weave_host_plan seen
	{
	};
	std::vector<uint8_t> res_copy;
	size_t res_pitch = 0;
	before_complete = [&]() {
		seen = hp.last;
		if (!seen.result) {
			return;
		}
		IOSurfaceRef s = (IOSurfaceRef)seen.result_iosurface;
		IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr);
		res_pitch = IOSurfaceGetBytesPerRow(s);
		const uint8_t *base = (const uint8_t *)IOSurfaceGetBaseAddress(s);
		res_copy.assign(base, base + res_pitch * seen.result_h);
		IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
	};
	REQUIRE(weave());
	before_complete = nullptr;
	REQUIRE(seen.result);
	CHECK((seen.result_format == XRT_CONV_FMT_RGBA8_UNORM || seen.result_format == XRT_CONV_FMT_RGBA8_SRGB ||
	       seen.result_format == XRT_CONV_FMT_BGRA8_UNORM || seen.result_format == XRT_CONV_FMT_BGRA8_SRGB));
	CHECK(IOSurfaceGetPixelFormat((IOSurfaceRef)seen.result_iosurface) == 0u); // why the plan carries the format
	{
		// Step E: the composed views were handed over on the glue's shared
		// event; the Metal pass GPU-waited on it (no host stall before publish).
		vk_conv_gpu_caps caps{};
		vk_conv_gpu_get_caps(rig.x.g, &caps);
		CHECK((seen.result_event != nullptr) == caps.async_s5);
		CHECK((seen.result_event_value > 0) == caps.async_s5);
	}
	REQUIRE(vk_conv_weave_client_stats(hp.c, wire, &st));
	CHECK(st.woven_result);
	CHECK(st.result_views == 2u);
	CHECK(st.committed == 2u);
	REQUIRE(cmw_rig_read(mr, sbs, out.data()));
	{
		const bool bgra =
		    seen.result_format == XRT_CONV_FMT_BGRA8_UNORM || seen.result_format == XRT_CONV_FMT_BGRA8_SRGB;
		const uint32_t vw = seen.result_w / seen.result_views;
		const float aw = (float)(seen.ax1 - seen.ax0), ah = (float)(seen.ay1 - seen.ay0);
		uint32_t checked = 0, close = 0;
		for (uint32_t tile = 0; tile < 2; tile++) {
			const uint32_t sv = seen.tile_view[tile];
			for (uint32_t gy = 1; gy < 8; gy++) {
				for (uint32_t gx = 1; gx < 8; gx++) {
					const uint32_t dx = (uint32_t)seen.ax0 + (uint32_t)(aw * gx / 8.0f);
					const uint32_t dy = (uint32_t)seen.ay0 + (uint32_t)(ah * gy / 8.0f);
					const float u = ((float)(dx - seen.ax0) + 0.5f) / aw * (float)vw;
					const float v = ((float)(dy - seen.ay0) + 0.5f) / ah * (float)seen.result_h;
					const uint8_t *d = px(tile * W + dx, dy); // B G R A
					const int dmap[4] = {2, 1, 0, 3};
					bool ok = true;
					for (int ch = 0; ch < 4; ch++) {
						const float e = iosurface_bilinear(res_copy.data(), res_pitch, bgra, u,
						                                   v, sv * vw, vw, seen.result_h, ch);
						ok = ok && std::fabs(e - (float)d[dmap[ch]]) <= 2.0f;
					}
					checked++;
					close += ok ? 1 : 0;
				}
			}
		}
		CHECK(close == checked);
	}
	CHECK(px(10, 10)[3] == 0); // still nothing outside the rect
	CHECK(px(RX - 1, RY + 10)[3] == 0);
	CHECK(px(W + RX + RW, RY + 10)[3] == 0);

	// 3. The letterbox crop settles: the snapshot is the active area only, capped.
	bool cropped = false;
	for (int i = 0; i < 120 && !cropped; i++) {
		REQUIRE(weave());
		REQUIRE(vk_conv_weave_client_stats(hp.c, wire, &st));
		cropped = st.active_h == RH - 2 * BAR;
	}
	REQUIRE(cropped);
	CHECK(st.active_w == RW);
	CHECK(st.snap_w == 1920u);
	CHECK(st.snap_h == 640u);
	// Once a result of the cropped input lands, the bars are the 2D input, flat.
	const uint64_t done = ss.converted;
	REQUIRE(wait_until(
	    [&] {
		    return conv_core_stream_get_stats(rig.core, core_id, &ss) == XRT_SUCCESS && ss.converted > done + 1;
	    },
	    5000));
	REQUIRE(weave());
	REQUIRE(cmw_rig_read(mr, sbs, out.data()));
	CHECK(src_eq(580, RY + 50, px(580, RY + 50)));
	CHECK(src_eq(580, RY + 50, px(W + 580, RY + 50)));
	CHECK(comp_multi_weave_metal_lift_stream_count(ml) == 1u);

	comp_multi_weave_metal_lift_destroy(&ml);
	vk_conv_weave_client_destroy(w, hp.c);
	vk_conv_weave_destroy(&w);
	cmw_rig_release(in_tex);
	cmw_rig_release(sbs);
	cmw_rig_destroy(mr);
	CHECK(ipc_server_conv_lift_stream_destroy(sc, 1, wire) == XRT_SUCCESS);
}

TEST_CASE("Metal weave backend: a stream destroyed with a weave in flight keeps its images until complete",
          "[conv_vk_gpu][weave_rects][metal]")
{
	WeaveRig rig;
	if (!rig.init()) {
		SKIP("no Vulkan device / CPU transport available");
	}
	cmw_rig *mr = cmw_rig_create();
	if (mr == nullptr) {
		SKIP("no Metal device");
	}
	ipc_server_conv *sc = rig.sc;
	ipc_server_conv_set_weave_rects(sc, true);
	uint64_t wire = 0;
	REQUIRE(ipc_server_conv_lift_stream_create(sc, 1, XRT_DP_LIFT_MODE_SBS, 0, 1.0f, 0, 0, &wire) == XRT_SUCCESS);
	uint64_t core_id = 0;
	uint32_t mode = 0;
	REQUIRE(weave_resolve(sc, 1, wire, &core_id, &mode));
	vk_conv_weave *w = nullptr;
	REQUIRE(vk_conv_weave_create(rig.x.vk.get(), rig.x.g, &w));
	vk_conv_weave_bridge b{};
	b.core = rig.core;
	b.resolve = weave_resolve;
	b.match_weave = weave_match;
	b.fit_rect = ipc_server_conv_submit_rect;
	b.ud = sc;
	vk_conv_weave_set_bridge(w, &b);
	HostPlanner hp;
	hp.w = w;
	hp.c = vk_conv_weave_client_create(w, nullptr);
	REQUIRE(hp.c != nullptr);
	CHECK_FALSE(vk_conv_weave_record(w, hp.c, (VkCommandBuffer)1, nullptr)); // a host client never records Vulkan
	comp_multi_weave_metal_lift *ml = comp_multi_weave_metal_lift_create(cmw_rig_device(mr));
	REQUIRE(ml != nullptr);
	const comp_multi_weave_metal_lift_planner pl = hp.get();

	const uint32_t W = 640, H = 360;
	std::vector<uint8_t> src((size_t)W * H * 4, 0x70);
	void *in_tex = cmw_rig_texture(mr, W, H, true, src.data());
	void *sbs = cmw_rig_texture(mr, 2 * W, H, true, nullptr);
	REQUIRE(in_tex != nullptr);
	REQUIRE(sbs != nullptr);

	xrt_lift_weave_rect binding{};
	binding.stream_id = wire;
	vk_conv_weave_rect r{};
	r.binding = &binding;
	r.owner = 1;
	r.src = {VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_FORMAT_B8G8R8A8_UNORM, W, H};
	r.x = 64;
	r.y = 32;
	r.w = 320;
	r.h = 180;
	r.dst = {VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_FORMAT_B8G8R8A8_UNORM, 2 * W, H};
	r.views = 2;
	r.tile_columns = 2;
	r.tile_w = W;
	r.tile_h = H;
	r.batch = true;
	r.frame.valid = false;
	r.frame.width_m = 0.16f;
	r.frame.height_m = 0.09f;

	// Until a result is woven; that frame's complete is withheld (in flight).
	vk_conv_weave_stats st{};
	bool woven = false;
	for (int i = 0; i < 200 && !woven; i++) {
		vk_conv_weave_begin(w, hp.c);
		REQUIRE(cmw_rig_weave(mr, ml, &pl, in_tex, sbs, &r, 1) == 1);
		REQUIRE(vk_conv_weave_client_stats(hp.c, wire, &st));
		woven = st.woven_result;
		if (!woven) {
			vk_conv_weave_complete(w, hp.c);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}
	REQUIRE(woven);
	const uint32_t images = vk_conv_gpu_image_count(rig.x.g);
	REQUIRE(images > 0u);
	CHECK(ipc_server_conv_lift_stream_destroy(sc, 1, wire) == XRT_SUCCESS);
	CHECK_FALSE(weave_resolve(sc, 1, wire, &core_id, &mode));
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	CHECK(vk_conv_gpu_image_count(rig.x.g) == images); // pinned: the reaper skips it

	vk_conv_weave_complete(w, hp.c);
	CHECK(wait_until([&] { return vk_conv_gpu_image_count(rig.x.g) == 0u; }, 3000));
	CHECK_FALSE(vk_conv_weave_client_stats(hp.c, wire, &st));

	// The next weave of the gone stream: flat, no plan pins anything.
	vk_conv_weave_begin(w, hp.c);
	REQUIRE(cmw_rig_weave(mr, ml, &pl, in_tex, sbs, &r, 1) == 1);
	CHECK_FALSE(hp.last.result);
	CHECK_FALSE(hp.last.snapshot);
	vk_conv_weave_complete(w, hp.c);

	comp_multi_weave_metal_lift_destroy(&ml);
	vk_conv_weave_client_destroy(w, hp.c);
	vk_conv_weave_destroy(&w);
	cmw_rig_release(in_tex);
	cmw_rig_release(sbs);
	cmw_rig_destroy(mr);
}

#endif // CONV_TEST_HAVE_METAL_WEAVE
