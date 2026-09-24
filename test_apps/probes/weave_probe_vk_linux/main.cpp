// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_weave desktop-Linux probe (#1699 R2) — headless present-owner
 *         harness for the stage-A (OPAQUE_FD, synchronous) weave contract.
 *
 * The Linux sibling of weave_probe_vk_macos (#759), reduced to the pure
 * service contract — no window, no swapchain, no presentation, CPU-verifiable:
 *
 *   1. Headless Vulkan device (the one the runtime names via
 *      XR_KHR_vulkan_enable) and a forced-IPC OpenXR session. Enabling
 *      XR_DXR_weave makes the client a PRESENT_OWNER.
 *   2. xrWeaveBindWindow2DXR(fake id) with an explicit XrWeaveWindowGeometryDXR
 *      (stage A infers the input extent from it, and the service feeds its
 *      origin to the DP phase slot).
 *   3. Vulkan-renders (buffer upload) a window-sized BGRA input into an
 *      exportable OPAQUE_FD image with two squeezed-SBS rects:
 *        rect A: left eye WHITE, right eye BLACK  -> anaglyph weave = RED
 *        rect B: left eye BLACK, right eye WHITE  -> anaglyph weave = CYAN
 *      (sim_display anaglyph.frag: out = (left.r, right.g, right.b, ...).)
 *      A second, identical input is allocated too; the long run alternates the
 *      two, exercising the service's multi-slot import cache.
 *   4. Batched xrWeaveSubmitDXR (spec v3) + v4 overlay (magenta bar) + v5
 *      firstChunk. The first submit hands back the woven output as an
 *      OPAQUE_FD; the probe imports it on its own device and reads it back.
 *   5. N frames (default 600): the fd counts of this process and (with
 *      --service-pid) of the service must stay flat.
 *   6. Spec-v6 N-view atlas (2x1 red|cyan, zero-copy) -> WHITE.
 *
 * Stage-A image contract (both sides, identical creation parameters — a
 * requirement of a dedicated OPAQUE_FD import): VK_FORMAT_B8G8R8A8_UNORM, 2D,
 * 1 mip, 1 layer, TILING_OPTIMAL, usage COLOR_ATTACHMENT | SAMPLED |
 * TRANSFER_SRC | TRANSFER_DST, dedicated allocation, exported OPAQUE_FD; the
 * producer releases the image to VK_QUEUE_FAMILY_EXTERNAL in GENERAL, and
 * acquires the output from it in GENERAL.
 *
 * --dmabuf (stage B, spec v10): inputs + overlay are exportable dma-bufs with
 * the driver's DRM format modifier (input[1] in DRM ABGR8888 byte order, so both
 * fourcc mappings run), handed over with XrWeaveDmabufDescDXR /
 * XrWeaveOverlayDmabufDescDXR and a LIVE acquire sync_file (the probe's upload
 * is not CPU-waited: the service's GPU wait is what orders its read). The two
 * inputs carry OPPOSITE patterns and alternate, the woven output comes back as
 * an XrWeaveOutputDmabufDXR (imported with its explicit modifier) and every
 * frame is read back after waiting that frame's XrWeaveOutputSyncDXR release
 * sync_file ON THE GPU, then checked against that frame's parity.
 * --no-release-wait skips that wait: the negative control, which must FAIL.
 * --linear forces LINEAR inputs.
 *
 * Run (service already started with the sim display plug-in):
 *   SIM_DISPLAY_OUTPUT=anaglyph XRT_PLUGIN_SEARCH_PATH=build/_plugins \
 *     build/src/xrt/targets/service/displayxr-service &
 *   XRT_FORCE_MODE=ipc XR_RUNTIME_JSON=build/openxr_displayxr-dev.json \
 *     ./weave_probe_vk_linux --service-pid=$!
 */

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_DXR_weave.h>
#include <openxr/XR_DXR_view_rig.h>

#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

#define VK_CHECK_B(call)                                                                                               \
	do {                                                                                                           \
		VkResult _v = (call);                                                                                  \
		if (_v != VK_SUCCESS) {                                                                                \
			LOG("FAILED %s -> %d", #call, (int)_v);                                                        \
			return false;                                                                                  \
		}                                                                                                      \
	} while (0)

// Window-client-sized input (the v3 batch contract) + the two weaved rects.
static const uint32_t kWinW = 1280;
static const uint32_t kWinH = 720;
static const XrRect2Di kRectA = {{100, 100}, {320, 180}};
static const XrRect2Di kRectB = {{700, 400}, {400, 200}};
// v4 overlay: an opaque magenta bar in a gap that overlaps neither rect.
static const XrRect2Di kOverlayBar = {{200, 30}, {800, 40}};
// A fake on-screen origin for the bound window (fed to the DP phase slot).
static const XrOffset2Di kWinOrigin = {64, 48};

static const VkFormat kFormat = VK_FORMAT_B8G8R8A8_UNORM;
static const VkImageUsageFlags kUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

struct Vk
{
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t qfi = 0;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	PFN_vkGetMemoryFdKHR get_memory_fd = nullptr;
	VkPhysicalDeviceMemoryProperties mem_props = {};
};

struct Image
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	uint32_t w = 0, h = 0;
	int fd = -1; //!< Exported OPAQUE_FD (inputs only); the probe keeps it for the whole run.
};

struct Buffer
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void *map = nullptr;
	VkDeviceSize size = 0;
};

static int
count_fds(long pid)
{
	char path[64];
	if (pid <= 0) {
		snprintf(path, sizeof(path), "/proc/self/fd");
	} else {
		snprintf(path, sizeof(path), "/proc/%ld/fd", pid);
	}
	DIR *d = opendir(path);
	if (d == nullptr) {
		return -1;
	}
	int n = 0;
	while (struct dirent *e = readdir(d)) {
		if (e->d_name[0] != '.') {
			n++;
		}
	}
	closedir(d);
	return n - (pid <= 0 ? 1 : 0); // our own opendir fd
}

static bool
find_memory_type(const Vk &vk, uint32_t bits, VkMemoryPropertyFlags want, uint32_t *out)
{
	for (uint32_t i = 0; i < vk.mem_props.memoryTypeCount; i++) {
		if ((bits & (1u << i)) && (vk.mem_props.memoryTypes[i].propertyFlags & want) == want) {
			*out = i;
			return true;
		}
	}
	return false;
}

static bool
create_buffer(Vk &vk, VkDeviceSize size, Buffer &out)
{
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = size;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VK_CHECK_B(vkCreateBuffer(vk.device, &bci, nullptr, &out.buffer));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(vk.device, out.buffer, &req);
	uint32_t type = 0;
	if (!find_memory_type(vk, req.memoryTypeBits,
	                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type)) {
		LOG("no host-visible memory type");
		return false;
	}
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = type;
	VK_CHECK_B(vkAllocateMemory(vk.device, &mai, nullptr, &out.memory));
	VK_CHECK_B(vkBindBufferMemory(vk.device, out.buffer, out.memory, 0));
	VK_CHECK_B(vkMapMemory(vk.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.map));
	out.size = size;
	return true;
}

static void
destroy_buffer(Vk &vk, Buffer &b)
{
	if (b.map != nullptr) {
		vkUnmapMemory(vk.device, b.memory);
	}
	vkDestroyBuffer(vk.device, b.buffer, nullptr);
	vkFreeMemory(vk.device, b.memory, nullptr);
	b = Buffer{};
}

static VkImageCreateInfo
contract_image_info(uint32_t w, uint32_t h, const VkExternalMemoryImageCreateInfo *ext)
{
	VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.pNext = ext;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = kFormat;
	ici.extent = {w, h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = kUsage;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	return ici;
}

//! Exportable OPAQUE_FD image per the stage-A contract; exports ONE fd.
static bool
create_exported_image(Vk &vk, uint32_t w, uint32_t h, Image &out)
{
	VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	VkImageCreateInfo ici = contract_image_info(w, h, &ext);
	VK_CHECK_B(vkCreateImage(vk.device, &ici, nullptr, &out.image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(vk.device, out.image, &req);
	uint32_t type = 0;
	if (!find_memory_type(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
		LOG("no device-local memory type");
		return false;
	}
	VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	ded.image = out.image;
	VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
	exp.pNext = &ded;
	exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext = &exp;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = type;
	VK_CHECK_B(vkAllocateMemory(vk.device, &mai, nullptr, &out.memory));
	VK_CHECK_B(vkBindImageMemory(vk.device, out.image, out.memory, 0));
	VkMemoryGetFdInfoKHR gfi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	gfi.memory = out.memory;
	gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	VK_CHECK_B(vk.get_memory_fd(vk.device, &gfi, &out.fd));
	out.w = w;
	out.h = h;
	return true;
}

//! Import the service's woven output (OPAQUE_FD, identical parameters). Consumes @p fd on success.
static bool
import_output_image(Vk &vk, int fd, uint32_t w, uint32_t h, Image &out)
{
	VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	VkImageCreateInfo ici = contract_image_info(w, h, &ext);
	VK_CHECK_B(vkCreateImage(vk.device, &ici, nullptr, &out.image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(vk.device, out.image, &req);
	uint32_t type = 0;
	if (!find_memory_type(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
		return false;
	}
	VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	ded.image = out.image;
	VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
	imp.pNext = &ded;
	imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	imp.fd = fd;
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext = &imp;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = type;
	VK_CHECK_B(vkAllocateMemory(vk.device, &mai, nullptr, &out.memory));
	VK_CHECK_B(vkBindImageMemory(vk.device, out.image, out.memory, 0));
	out.w = w;
	out.h = h;
	return true;
}

static void
destroy_image(Vk &vk, Image &img)
{
	vkDestroyImage(vk.device, img.image, nullptr);
	vkFreeMemory(vk.device, img.memory, nullptr);
	if (img.fd >= 0) {
		close(img.fd);
	}
	img = Image{};
}

static bool
begin_cmd(Vk &vk)
{
	VK_CHECK_B(vkResetCommandBuffer(vk.cmd, 0));
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK_B(vkBeginCommandBuffer(vk.cmd, &bi));
	return true;
}

static bool
submit_and_wait(Vk &vk)
{
	VK_CHECK_B(vkEndCommandBuffer(vk.cmd));
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &vk.cmd;
	VK_CHECK_B(vkQueueSubmit(vk.queue, 1, &si, vk.fence));
	VK_CHECK_B(vkWaitForFences(vk.device, 1, &vk.fence, VK_TRUE, 5ULL * 1000 * 1000 * 1000));
	VK_CHECK_B(vkResetFences(vk.device, 1, &vk.fence));
	return true;
}

static void
barrier(Vk &vk,
        VkImage image,
        VkImageLayout old_l,
        VkImageLayout new_l,
        VkAccessFlags src_a,
        VkAccessFlags dst_a,
        VkPipelineStageFlags src_s,
        VkPipelineStageFlags dst_s,
        uint32_t src_q = VK_QUEUE_FAMILY_IGNORED,
        uint32_t dst_q = VK_QUEUE_FAMILY_IGNORED)
{
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = src_a;
	b.dstAccessMask = dst_a;
	b.oldLayout = old_l;
	b.newLayout = new_l;
	b.srcQueueFamilyIndex = src_q;
	b.dstQueueFamilyIndex = dst_q;
	b.image = image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(vk.cmd, src_s, dst_s, 0, 0, nullptr, 0, nullptr, 1, &b);
}

//! Upload BGRA @p pixels into @p img and hand it to the service: GENERAL, released to EXTERNAL.
static bool
upload_and_release(Vk &vk, Image &img, const std::vector<uint8_t> &pixels)
{
	Buffer staging;
	if (!create_buffer(vk, pixels.size(), staging)) {
		return false;
	}
	memcpy(staging.map, pixels.data(), pixels.size());
	if (!begin_cmd(vk)) {
		return false;
	}
	barrier(vk, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkBufferImageCopy region = {};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {img.w, img.h, 1};
	vkCmdCopyBufferToImage(vk.cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	barrier(vk, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
	        VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	// Queue-family release to the service (stage-A contract: GENERAL both sides).
	barrier(vk, img.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
	        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, vk.qfi, VK_QUEUE_FAMILY_EXTERNAL);
	bool ok = submit_and_wait(vk);
	destroy_buffer(vk, staging);
	return ok;
}

//! Read the woven output back (acquire from EXTERNAL, copy, release back).
static bool
readback(Vk &vk, Image &img, std::vector<uint8_t> &out)
{
	Buffer rb;
	if (!create_buffer(vk, (VkDeviceSize)img.w * img.h * 4, rb)) {
		return false;
	}
	if (!begin_cmd(vk)) {
		return false;
	}
	barrier(vk, img.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
	        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_QUEUE_FAMILY_EXTERNAL, vk.qfi);
	VkBufferImageCopy region = {};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {img.w, img.h, 1};
	vkCmdCopyImageToBuffer(vk.cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, rb.buffer, 1, &region);
	barrier(vk, img.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, 0,
	        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, vk.qfi, VK_QUEUE_FAMILY_EXTERNAL);
	bool ok = submit_and_wait(vk);
	if (ok) {
		out.assign((const uint8_t *)rb.map, (const uint8_t *)rb.map + rb.size);
	}
	destroy_buffer(vk, rb);
	return ok;
}

static void
put_px(std::vector<uint8_t> &px, uint32_t w, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 0xFF)
{
	uint8_t *p = px.data() + ((size_t)y * w + (size_t)x) * 4;
	p[0] = b;
	p[1] = g;
	p[2] = r;
	p[3] = a;
}

static void
fill_sbs_rect(std::vector<uint8_t> &px, uint32_t w, const XrRect2Di &r, uint8_t left_lum, uint8_t right_lum)
{
	int half = r.extent.width / 2;
	for (int y = r.offset.y; y < r.offset.y + r.extent.height; y++) {
		for (int x = r.offset.x; x < r.offset.x + r.extent.width; x++) {
			uint8_t lum = (x - r.offset.x) < half ? left_lum : right_lum;
			put_px(px, w, x, y, lum, lum, lum);
		}
	}
}

static void
sample(const std::vector<uint8_t> &px, uint32_t w, int x, int y, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a)
{
	const uint8_t *p = px.data() + ((size_t)y * w + (size_t)x) * 4;
	*b = p[0];
	*g = p[1];
	*r = p[2];
	*a = p[3];
}

static void
dump_ppm(const std::vector<uint8_t> &px, uint32_t w, uint32_t h, const std::string &path)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (f == nullptr) {
		return;
	}
	fprintf(f, "P6\n%u %u\n255\n", w, h);
	for (size_t i = 0; i < (size_t)w * h; i++) {
		uint8_t rgb[3] = {px[i * 4 + 2], px[i * 4 + 1], px[i * 4 + 0]};
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	LOG("dumped woven output -> %s (%ux%u)", path.c_str(), w, h);
}

static void
split_exts(const std::string &s, std::vector<std::string> &storage)
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
}

static void
barrier_cmd(VkCommandBuffer cmd,
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

/*
 *
 * Stage B (--dmabuf): dma-buf inputs with DRM format modifiers, sync_file
 * acquire / release fences, the woven output as a typed dma-buf.
 *
 * The probe links the OpenXR loader + Vulkan only, so the few calls R5's
 * aux_vk helpers make are replicated here (same shapes as vk_dmabuf.c).
 *
 */

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif
#define PROBE_FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
static const uint32_t kFourccARGB8888 = PROBE_FOURCC('A', 'R', '2', '4'); // bytes B,G,R,A = VK B8G8R8A8
static const uint32_t kFourccABGR8888 = PROBE_FOURCC('A', 'B', '2', '4'); // bytes R,G,B,A = VK R8G8B8A8

struct DmabufFns
{
	PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier = nullptr;
	PFN_vkGetMemoryFdPropertiesKHR get_fd_props = nullptr;
	PFN_vkImportSemaphoreFdKHR import_sem_fd = nullptr;
	PFN_vkGetSemaphoreFdKHR get_sem_fd = nullptr;
};

struct DmabufImage
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	uint32_t w = 0, h = 0;
	VkFormat format = VK_FORMAT_UNDEFINED;
	uint32_t fourcc = 0;
	uint64_t modifier = 0;
	uint32_t offset = 0, stride = 0;
	int fd = -1;           //!< The probe's own dma-buf fd (inputs); a dup() is handed over per submit.
	bool owned_by_us = true; //!< false once released to VK_QUEUE_FAMILY_FOREIGN_EXT
};

static const VkImageUsageFlags kDmabufUsage =
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

//! Modifiers the device can EXPORT for @p format + kDmabufUsage (vkGetPhysicalDeviceImageFormatProperties2).
static std::vector<uint64_t>
exportable_modifiers(Vk &vk, VkFormat format)
{
	VkDrmFormatModifierPropertiesListEXT list = {VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
	VkFormatProperties2 fp = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
	fp.pNext = &list;
	vkGetPhysicalDeviceFormatProperties2(vk.phys, format, &fp);
	std::vector<VkDrmFormatModifierPropertiesEXT> props(list.drmFormatModifierCount);
	list.pDrmFormatModifierProperties = props.data();
	vkGetPhysicalDeviceFormatProperties2(vk.phys, format, &fp);
	std::vector<uint64_t> out;
	for (const auto &p : props) {
		VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {
		    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
		mi.drmFormatModifier = p.drmFormatModifier;
		mi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
		ei.pNext = &mi;
		ei.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		VkPhysicalDeviceImageFormatInfo2 ii = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
		ii.pNext = &ei;
		ii.format = format;
		ii.type = VK_IMAGE_TYPE_2D;
		ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
		ii.usage = kDmabufUsage;
		VkExternalImageFormatProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
		VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
		ip.pNext = &ep;
		if (vkGetPhysicalDeviceImageFormatProperties2(vk.phys, &ii, &ip) != VK_SUCCESS) {
			continue;
		}
		if (!(ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
			continue;
		}
		if (p.drmFormatModifierPlaneCount != 1) {
			continue; // the probe describes one memory plane
		}
		out.push_back(p.drmFormatModifier);
	}
	return out;
}

//! Exportable dma-buf image, driver-picked modifier (or LINEAR); exports ONE fd.
static bool
create_dmabuf_image(Vk &vk, const DmabufFns &fn, uint32_t w, uint32_t h, VkFormat format, uint32_t fourcc,
                    bool force_linear, DmabufImage &out)
{
	std::vector<uint64_t> mods;
	if (!force_linear) {
		mods = exportable_modifiers(vk, format);
	}
	if (mods.empty()) {
		mods.push_back(DRM_FORMAT_MOD_LINEAR);
	}
	VkImageDrmFormatModifierListCreateInfoEXT ml = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
	ml.drmFormatModifierCount = (uint32_t)mods.size();
	ml.pDrmFormatModifiers = mods.data();
	VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	ext.pNext = &ml;
	ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.pNext = &ext;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = format;
	ici.extent = {w, h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	ici.usage = kDmabufUsage;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK_B(vkCreateImage(vk.device, &ici, nullptr, &out.image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(vk.device, out.image, &req);
	uint32_t type = 0;
	if (!find_memory_type(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
		LOG("no device-local memory type for the dma-buf");
		return false;
	}
	VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	ded.image = out.image;
	VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
	exp.pNext = &ded;
	exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext = &exp;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = type;
	VK_CHECK_B(vkAllocateMemory(vk.device, &mai, nullptr, &out.memory));
	VK_CHECK_B(vkBindImageMemory(vk.device, out.image, out.memory, 0));

	VkImageDrmFormatModifierPropertiesEXT mp = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
	VK_CHECK_B(fn.get_modifier(vk.device, out.image, &mp));
	VkImageSubresource sub = {VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0};
	VkSubresourceLayout layout = {};
	vkGetImageSubresourceLayout(vk.device, out.image, &sub, &layout);

	VkMemoryGetFdInfoKHR gfi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	gfi.memory = out.memory;
	gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	VK_CHECK_B(vk.get_memory_fd(vk.device, &gfi, &out.fd));
	out.w = w;
	out.h = h;
	out.format = format;
	out.fourcc = fourcc;
	out.modifier = mp.drmFormatModifier;
	out.offset = (uint32_t)layout.offset;
	out.stride = (uint32_t)layout.rowPitch;
	out.owned_by_us = true;
	return true;
}

//! Import the service's woven dma-buf output through its descriptor (explicit modifier). Consumes d.fd on success.
static bool
import_dmabuf_output(Vk &vk, const DmabufFns &fn, const XrWeaveOutputDmabufDXR &d, DmabufImage &out)
{
	VkFormat format = d.drmFourcc == kFourccABGR8888 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
	std::vector<VkSubresourceLayout> planes(d.planeCount);
	for (uint32_t i = 0; i < d.planeCount; i++) {
		planes[i] = {};
		planes[i].offset = d.offsets[i];
		planes[i].rowPitch = d.strides[i];
	}
	VkImageDrmFormatModifierExplicitCreateInfoEXT ex = {
	    VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
	ex.drmFormatModifier = d.drmModifier;
	ex.drmFormatModifierPlaneCount = d.planeCount;
	ex.pPlaneLayouts = planes.data();
	VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	ext.pNext = &ex;
	ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.pNext = &ext;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = format;
	ici.extent = {d.width, d.height, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK_B(vkCreateImage(vk.device, &ici, nullptr, &out.image));
	VkMemoryFdPropertiesKHR fdp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
	VK_CHECK_B(fn.get_fd_props(vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, d.fd, &fdp));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(vk.device, out.image, &req);
	uint32_t type = 0;
	if (!find_memory_type(vk, req.memoryTypeBits & fdp.memoryTypeBits, 0, &type)) {
		LOG("no memory type can import the woven dma-buf");
		return false;
	}
	VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	ded.image = out.image;
	VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
	imp.pNext = &ded;
	imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	imp.fd = d.fd;
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext = &imp;
	mai.allocationSize = req.size > d.size ? req.size : d.size;
	mai.memoryTypeIndex = type;
	VK_CHECK_B(vkAllocateMemory(vk.device, &mai, nullptr, &out.memory));
	VK_CHECK_B(vkBindImageMemory(vk.device, out.image, out.memory, 0));
	out.w = d.width;
	out.h = d.height;
	out.format = format;
	out.fourcc = d.drmFourcc;
	out.modifier = d.drmModifier;
	return true;
}

static void
destroy_dmabuf_image(Vk &vk, DmabufImage &img)
{
	vkDestroyImage(vk.device, img.image, nullptr);
	vkFreeMemory(vk.device, img.memory, nullptr);
	if (img.fd >= 0) {
		close(img.fd);
	}
	img = DmabufImage{};
}

static bool
create_sync_fd_semaphore(Vk &vk, bool exportable, VkSemaphore *out)
{
	VkExportSemaphoreCreateInfo esci = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
	esci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
	VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	sci.pNext = exportable ? &esci : nullptr;
	VK_CHECK_B(vkCreateSemaphore(vk.device, &sci, nullptr, out));
	return true;
}

//! Per-frame recording state of the dma-buf mode: one upload cmd (signals the
//! acquire semaphore) and one readback cmd, each with its own fence.
struct DmabufCtx
{
	Vk *vk = nullptr;
	DmabufFns fn;
	VkCommandBuffer up_cmd = VK_NULL_HANDLE, rb_cmd = VK_NULL_HANDLE;
	VkFence up_fence = VK_NULL_HANDLE, rb_fence = VK_NULL_HANDLE;
	bool up_pending = false;
	VkSemaphore acq_sem = VK_NULL_HANDLE; //!< exportable SYNC_FD, signalled by the upload
	VkSemaphore rel_sem = VK_NULL_HANDLE; //!< the service's release sync_file is imported here
};

static void
foreign_barrier(VkCommandBuffer cmd, uint32_t qfi, VkImage image, bool acquire, VkImageLayout old_l,
                VkImageLayout new_l, VkAccessFlags access, VkPipelineStageFlags stage)
{
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = acquire ? 0 : access;
	b.dstAccessMask = acquire ? access : 0;
	b.oldLayout = old_l;
	b.newLayout = new_l;
	b.srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_FOREIGN_EXT : qfi;
	b.dstQueueFamilyIndex = acquire ? qfi : VK_QUEUE_FAMILY_FOREIGN_EXT;
	b.image = image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : stage,
	                     acquire ? stage : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static bool
begin(VkCommandBuffer cmd)
{
	VK_CHECK_B(vkResetCommandBuffer(cmd, 0));
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK_B(vkBeginCommandBuffer(cmd, &bi));
	return true;
}

/*!
 * "Render" @p img (a staging copy — the probe's stand-in for a GPU producer)
 * and hand it to the service: acquire from FOREIGN (after the first frame),
 * copy, release to FOREIGN in GENERAL, signal acq_sem, and export it as the
 * acquire sync_file — WITHOUT a CPU wait, so the service's GPU wait is what
 * orders its read after this write. @p out_fd is the fence (-1 if @p fence is false).
 */
static bool
dmabuf_upload(DmabufCtx &c, DmabufImage &img, const Buffer &staging, bool fence, int *out_fd)
{
	Vk &vk = *c.vk;
	*out_fd = -1;
	if (c.up_pending) {
		VK_CHECK_B(vkWaitForFences(vk.device, 1, &c.up_fence, VK_TRUE, 5ULL * 1000 * 1000 * 1000));
		VK_CHECK_B(vkResetFences(vk.device, 1, &c.up_fence));
		c.up_pending = false;
	}
	if (!begin(c.up_cmd)) {
		return false;
	}
	if (img.owned_by_us) {
		barrier_cmd(c.up_cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	} else {
		foreign_barrier(c.up_cmd, vk.qfi, img.image, true, VK_IMAGE_LAYOUT_GENERAL,
		                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		                VK_PIPELINE_STAGE_TRANSFER_BIT);
	}
	VkBufferImageCopy region = {};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {img.w, img.h, 1};
	vkCmdCopyBufferToImage(c.up_cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	foreign_barrier(c.up_cmd, vk.qfi, img.image, false, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	img.owned_by_us = false;
	VK_CHECK_B(vkEndCommandBuffer(c.up_cmd));
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &c.up_cmd;
	si.signalSemaphoreCount = fence ? 1 : 0;
	si.pSignalSemaphores = &c.acq_sem;
	VK_CHECK_B(vkQueueSubmit(vk.queue, 1, &si, c.up_fence));
	c.up_pending = true;
	if (fence) {
		VkSemaphoreGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
		gi.semaphore = c.acq_sem;
		gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		VK_CHECK_B(c.fn.get_sem_fd(vk.device, &gi, out_fd));
	} else {
		// No acquire fence on this submit: honour the v9 contract instead.
		VK_CHECK_B(vkWaitForFences(vk.device, 1, &c.up_fence, VK_TRUE, 5ULL * 1000 * 1000 * 1000));
		VK_CHECK_B(vkResetFences(vk.device, 1, &c.up_fence));
		c.up_pending = false;
	}
	return true;
}

/*!
 * Read the woven output back: wait the release sync_file ON THE GPU (import
 * into rel_sem, waited by the readback submit) unless @p release_fd is -1,
 * acquire from FOREIGN, copy, release back. @p release_fd is consumed.
 */
static bool
dmabuf_readback(DmabufCtx &c, DmabufImage &img, Buffer &rb, int release_fd)
{
	Vk &vk = *c.vk;
	bool wait = false;
	if (release_fd >= 0) {
		VkImportSemaphoreFdInfoKHR ii = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
		ii.semaphore = c.rel_sem;
		ii.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
		ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		ii.fd = release_fd;
		if (c.fn.import_sem_fd(vk.device, &ii) != VK_SUCCESS) {
			close(release_fd);
			LOG("FAIL: release sync_file import");
			return false;
		}
		wait = true; // Vulkan owns release_fd now
	}
	if (!begin(c.rb_cmd)) {
		return false;
	}
	foreign_barrier(c.rb_cmd, vk.qfi, img.image, true, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
	                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkBufferImageCopy region = {};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {img.w, img.h, 1};
	vkCmdCopyImageToBuffer(c.rb_cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, rb.buffer, 1, &region);
	foreign_barrier(c.rb_cmd, vk.qfi, img.image, false, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
	                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VK_CHECK_B(vkEndCommandBuffer(c.rb_cmd));
	const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.waitSemaphoreCount = wait ? 1 : 0;
	si.pWaitSemaphores = &c.rel_sem;
	si.pWaitDstStageMask = &stage;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &c.rb_cmd;
	VK_CHECK_B(vkQueueSubmit(vk.queue, 1, &si, c.rb_fence));
	VK_CHECK_B(vkWaitForFences(vk.device, 1, &c.rb_fence, VK_TRUE, 5ULL * 1000 * 1000 * 1000));
	VK_CHECK_B(vkResetFences(vk.device, 1, &c.rb_fence));
	return true;
}

//! Fill a staging buffer with a window-sized squeezed-SBS input in @p fourcc byte order.
static void
fill_input(Buffer &b, uint32_t fourcc, bool swapped)
{
	std::vector<uint8_t> px((size_t)kWinW * kWinH * 4);
	for (uint32_t y = 0; y < kWinH; y++) {
		for (uint32_t x = 0; x < kWinW; x++) {
			put_px(px, kWinW, (int)x, (int)y, 32, 32, 32);
		}
	}
	// parity 0: A red, B cyan; parity 1: swapped — so a stale output reads as the other parity.
	fill_sbs_rect(px, kWinW, kRectA, swapped ? 0x00 : 0xFF, swapped ? 0xFF : 0x00);
	fill_sbs_rect(px, kWinW, kRectB, swapped ? 0xFF : 0x00, swapped ? 0x00 : 0xFF);
	if (fourcc == kFourccABGR8888) {
		for (size_t i = 0; i < px.size(); i += 4) {
			std::swap(px[i + 0], px[i + 2]); // BGRA -> RGBA byte order
		}
	}
	memcpy(b.map, px.data(), px.size());
}

static void
fill_dmabuf_desc(XrWeaveDmabufDescDXR &d, const DmabufImage &img, int fd, uint64_t buffer_id)
{
	d.fd = fd;
	d.width = img.w;
	d.height = img.h;
	d.drmFourcc = img.fourcc;
	d.drmModifier = img.modifier;
	d.planeCount = 1;
	d.offsets[0] = img.offset;
	d.strides[0] = img.stride;
	d.bufferId = buffer_id;
}

/*!
 * The whole --dmabuf run. Two window-sized inputs (input[0] ARGB8888 = BGRA
 * bytes, input[1] ABGR8888 = RGBA bytes, so both fourcc mappings are exercised)
 * carry OPPOSITE patterns; they alternate per frame, each frame is re-rendered
 * into and handed over with a live acquire sync_file, and each frame's woven
 * output is read back after waiting that frame's release sync_file and checked
 * against that frame's parity. Without the release wait (--no-release-wait) a
 * readback that runs ahead of the weave sees the previous frame's parity.
 */
static bool
run_dmabuf(Vk &vk,
           XrSession session,
           PFN_xrWeaveSubmitDXR pfn_submit,
           int frames,
           long service_pid,
           bool release_wait,
           bool force_linear,
           const std::string &dump_dir)
{
	DmabufCtx c;
	c.vk = &vk;
	c.fn.get_modifier = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
	    vk.device, "vkGetImageDrmFormatModifierPropertiesEXT");
	c.fn.get_fd_props = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(vk.device, "vkGetMemoryFdPropertiesKHR");
	c.fn.import_sem_fd = (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(vk.device, "vkImportSemaphoreFdKHR");
	c.fn.get_sem_fd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(vk.device, "vkGetSemaphoreFdKHR");
	if (!c.fn.get_modifier || !c.fn.get_fd_props || !c.fn.import_sem_fd || !c.fn.get_sem_fd) {
		LOG("FAIL: dma-buf / sync_fd entry points unavailable on the probe device");
		return false;
	}
	{
		VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cai.commandPool = vk.pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		vkAllocateCommandBuffers(vk.device, &cai, &c.up_cmd);
		vkAllocateCommandBuffers(vk.device, &cai, &c.rb_cmd);
		VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		vkCreateFence(vk.device, &fci, nullptr, &c.up_fence);
		vkCreateFence(vk.device, &fci, nullptr, &c.rb_fence);
		if (!create_sync_fd_semaphore(vk, true, &c.acq_sem) || !create_sync_fd_semaphore(vk, false, &c.rel_sem)) {
			return false;
		}
	}

	bool pass = true;
	DmabufImage input[2], overlay;
	Buffer staging[2], ov_staging, rb;
	const uint32_t fourcc[2] = {kFourccARGB8888, kFourccABGR8888};
	const VkFormat vkfmt[2] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
	for (int i = 0; i < 2; i++) {
		if (!create_dmabuf_image(vk, c.fn, kWinW, kWinH, vkfmt[i], fourcc[i], force_linear, input[i]) ||
		    !create_buffer(vk, (VkDeviceSize)kWinW * kWinH * 4, staging[i])) {
			LOG("FAIL: dma-buf input setup");
			return false;
		}
		fill_input(staging[i], fourcc[i], /*swapped*/ i == 1);
		LOG("input[%d]: dma-buf fd %d, fourcc %.4s, modifier 0x%016llx, stride %u", i, input[i].fd,
		    (const char *)&input[i].fourcc, (unsigned long long)input[i].modifier, input[i].stride);
	}
	if (!create_dmabuf_image(vk, c.fn, kWinW, kWinH, VK_FORMAT_B8G8R8A8_UNORM, kFourccARGB8888, force_linear,
	                         overlay) ||
	    !create_buffer(vk, (VkDeviceSize)kWinW * kWinH * 4, ov_staging) ||
	    !create_buffer(vk, (VkDeviceSize)kWinW * kWinH * 4, rb)) {
		LOG("FAIL: overlay / readback setup");
		return false;
	}
	{
		std::vector<uint8_t> ov_px((size_t)kWinW * kWinH * 4, 0);
		for (int y = kOverlayBar.offset.y; y < kOverlayBar.offset.y + kOverlayBar.extent.height; y++) {
			for (int x = kOverlayBar.offset.x; x < kOverlayBar.offset.x + kOverlayBar.extent.width; x++) {
				put_px(ov_px, kWinW, x, y, 230, 13, 230, 255);
			}
		}
		memcpy(ov_staging.map, ov_px.data(), ov_px.size());
		int unused = -1;
		if (!dmabuf_upload(c, overlay, ov_staging, false, &unused)) {
			return false;
		}
	}

	XrRect2Di rects[2] = {kRectA, kRectB};
	DmabufImage output;
	uint64_t last_fv = 0;
	int probe_fds_early = -1, svc_fds_early = -1;
	int bad_frames = 0, neg_release = 0, extra_out_fds = 0;
	uint64_t out_modifier = 0;
	for (int frame = 0; frame < frames; frame++) {
		const int k = frame & 1;
		int acq_fd = -1;
		if (!dmabuf_upload(c, input[k], staging[k], true, &acq_fd)) {
			LOG("FAIL: upload frame %d", frame);
			return false;
		}

		XrWeaveDmabufDescDXR in_desc = {(XrStructureType)XR_TYPE_WEAVE_DMABUF_DESC_DXR};
		fill_dmabuf_desc(in_desc, input[k], dup(input[k].fd), (uint64_t)(k + 1)); // buffer-id keyed
		XrWeaveOverlayDmabufDescDXR ov_desc = {(XrStructureType)XR_TYPE_WEAVE_OVERLAY_DMABUF_DESC_DXR};
		fill_dmabuf_desc(*(XrWeaveDmabufDescDXR *)(void *)&ov_desc, overlay, dup(overlay.fd), 0); // inode keyed
		ov_desc.type = (XrStructureType)XR_TYPE_WEAVE_OVERLAY_DMABUF_DESC_DXR;
		XrWeaveSubmitSyncDXR sync = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_SYNC_DXR};
		sync.acquireFenceFd = acq_fd;
		XrWeaveSubmitOverlaysDXR ov = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_OVERLAYS_DXR};
		ov.rectCount = 0;
		XrWeaveSubmitRectsDXR batch = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_RECTS_DXR};
		batch.rectCount = 2;
		batch.rects = rects;
		// chain: submit -> batch -> ov -> in_desc -> ov_desc -> sync
		ov_desc.next = &sync;
		in_desc.next = &ov_desc;
		ov.next = &in_desc;
		batch.next = &ov;
		XrWeaveSubmitInfoDXR submit = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_INFO_DXR};
		submit.next = &batch;
		submit.firstChunk = XR_TRUE;

		XrWeaveOutputSyncDXR out_sync = {(XrStructureType)XR_TYPE_WEAVE_OUTPUT_SYNC_DXR};
		XrWeaveOutputDmabufDXR out_dmabuf = {(XrStructureType)XR_TYPE_WEAVE_OUTPUT_DMABUF_DXR};
		out_dmabuf.next = &out_sync;
		XrWeaveOutputDXR out = {(XrStructureType)XR_TYPE_WEAVE_OUTPUT_DXR};
		out.next = &out_dmabuf;
		XrResult r = pfn_submit(session, &submit, &out);
		if (XR_FAILED(r)) {
			// Not XR_SUCCESS: every fd we passed is still ours.
			close(in_desc.fd);
			close(ov_desc.fd);
			close(acq_fd);
			LOG("FAIL: xrWeaveSubmitDXR (dma-buf) frame %d -> %d", frame, (int)r);
			return false;
		}
		if (out_dmabuf.fd >= 0) {
			if (output.image != VK_NULL_HANDLE) {
				extra_out_fds++;
				close(out_dmabuf.fd);
			} else {
				LOG("frame %d: woven output dma-buf fd %d %ux%u fourcc %.4s modifier 0x%016llx planes %u "
				    "offset %u stride %u size %llu",
				    frame, out_dmabuf.fd, out_dmabuf.width, out_dmabuf.height,
				    (const char *)&out_dmabuf.drmFourcc, (unsigned long long)out_dmabuf.drmModifier,
				    out_dmabuf.planeCount, out_dmabuf.offsets[0], out_dmabuf.strides[0],
				    (unsigned long long)out_dmabuf.size);
				out_modifier = out_dmabuf.drmModifier;
				if (!import_dmabuf_output(vk, c.fn, out_dmabuf, output)) {
					close(out_dmabuf.fd);
					LOG("FAIL: could not import the woven dma-buf through its descriptor");
					return false;
				}
			}
		}
		if (output.image == VK_NULL_HANDLE) {
			LOG("FAIL: frame %d: no woven dma-buf yet", frame);
			if (out_sync.releaseFenceFd >= 0) {
				close(out_sync.releaseFenceFd);
			}
			return false;
		}
		if (out_sync.releaseFenceFd < 0) {
			neg_release++;
		}
		if (out.fenceValue <= last_fv) {
			LOG("FAIL: fenceValue not monotonic (%llu after %llu)", (unsigned long long)out.fenceValue,
			    (unsigned long long)last_fv);
			pass = false;
		}
		last_fv = out.fenceValue;
		if (frame == 0) {
			LOG("frame 0: woven %ux%u, release fence fd %d, fenceValue=%llu", out.width, out.height,
			    out_sync.releaseFenceFd, (unsigned long long)out.fenceValue);
		}

		int rel = out_sync.releaseFenceFd;
		if (!release_wait && rel >= 0) {
			close(rel); // negative control: read without waiting the weave
			rel = -1;
		}
		if (!dmabuf_readback(c, output, rb, rel)) {
			return false;
		}
		std::vector<uint8_t> px((const uint8_t *)rb.map, (const uint8_t *)rb.map + rb.size);
		uint8_t ar, ag, ab, aa, br, bg, bb, ba, vr, vg, vb, va, gr, gg, gb, ga;
		sample(px, kWinW, kRectA.offset.x + kRectA.extent.width / 2, kRectA.offset.y + kRectA.extent.height / 2,
		       &ar, &ag, &ab, &aa);
		sample(px, kWinW, kRectB.offset.x + kRectB.extent.width / 2, kRectB.offset.y + kRectB.extent.height / 2,
		       &br, &bg, &bb, &ba);
		sample(px, kWinW, kOverlayBar.offset.x + kOverlayBar.extent.width / 2,
		       kOverlayBar.offset.y + kOverlayBar.extent.height / 2, &vr, &vg, &vb, &va);
		sample(px, kWinW, 10, 10, &gr, &gg, &gb, &ga);
		auto red = [](uint8_t r, uint8_t g, uint8_t b) { return r > 150 && g < 80 && b < 80; };
		auto cyan = [](uint8_t r, uint8_t g, uint8_t b) { return r < 80 && g > 150 && b > 150; };
		const bool a_ok = k == 0 ? red(ar, ag, ab) : cyan(ar, ag, ab);
		const bool b_ok = k == 0 ? cyan(br, bg, bb) : red(br, bg, bb);
		const bool ov_ok = vr > 150 && vb > 150 && vg < 80 && va > 200;
		const bool gap_ok = ga < 40 && aa > 200;
		const bool ok = a_ok && b_ok && ov_ok && gap_ok;
		if (!ok) {
			if (bad_frames < 5) {
				LOG("frame %d (parity %d) WRONG: rectA (%u,%u,%u) rectB (%u,%u,%u) bar (%u,%u,%u,a=%u) "
				    "gap a=%u",
				    frame, k, ar, ag, ab, br, bg, bb, vr, vg, vb, va, ga);
			}
			bad_frames++;
		}
		if (frame == 0 || frame == 1) {
			LOG("frame %d (parity %d): rectA (%u,%u,%u) rectB (%u,%u,%u) bar (%u,%u,%u,a=%u) gap a=%u -> %s",
			    frame, k, ar, ag, ab, br, bg, bb, vr, vg, vb, va, ga, ok ? "OK" : "WRONG");
			if (frame == 0) {
				dump_ppm(px, kWinW, kWinH, dump_dir + "/weave_probe_linux_dmabuf_output.ppm");
			}
		}
		if (frame == 10) {
			probe_fds_early = count_fds(0);
			svc_fds_early = service_pid > 0 ? count_fds(service_pid) : -1;
		}
	}
	int probe_fds_late = count_fds(0);
	int svc_fds_late = service_pid > 0 ? count_fds(service_pid) : -1;
	LOG("dma-buf: %d frames, %d with a wrong pixel (release wait %s), release fd < 0 on %d frame(s), %d extra "
	    "output fd(s); output modifier 0x%016llx",
	    frames, bad_frames, release_wait ? "ON" : "OFF (negative control)", neg_release, extra_out_fds,
	    (unsigned long long)out_modifier);
	LOG("fd counts over %d frames: probe %d -> %d, service(pid %ld) %d -> %d", frames, probe_fds_early,
	    probe_fds_late, service_pid, svc_fds_early, svc_fds_late);
	if (probe_fds_late != probe_fds_early || svc_fds_late != svc_fds_early) {
		LOG("FAIL: fd count grew");
		pass = false;
	}
	if (neg_release > 0 || extra_out_fds > 0) {
		LOG("FAIL: release fd missing on %d frame(s) / %d unexpected output fd(s)", neg_release, extra_out_fds);
		pass = false;
	}
	if (bad_frames > 0) {
		LOG("%s: %d/%d frames had wrong pixels", release_wait ? "FAIL" : "negative control", bad_frames, frames);
		pass = false;
	}
	destroy_dmabuf_image(vk, output);

	// ---- v6 N-view atlas over dma-buf (2x1 red|cyan, zero-copy) -> WHITE.
	{
		const uint32_t cvw = 640, cvh = 360;
		DmabufImage nv;
		Buffer nv_staging;
		bool v6ok = create_dmabuf_image(vk, c.fn, 2 * cvw, cvh, VK_FORMAT_B8G8R8A8_UNORM, kFourccARGB8888,
		                                force_linear, nv) &&
		            create_buffer(vk, (VkDeviceSize)2 * cvw * cvh * 4, nv_staging);
		if (v6ok) {
			std::vector<uint8_t> nv_px((size_t)2 * cvw * cvh * 4);
			for (uint32_t y = 0; y < cvh; y++) {
				for (uint32_t x = 0; x < cvw; x++) {
					put_px(nv_px, 2 * cvw, (int)x, (int)y, 0xFF, 0x00, 0x00);
					put_px(nv_px, 2 * cvw, (int)(cvw + x), (int)y, 0x00, 0xFF, 0xFF);
				}
			}
			memcpy(nv_staging.map, nv_px.data(), nv_px.size());
		}
		DmabufImage v6out;
		int rel = -1;
		for (int frame = 0; v6ok && frame < 3; frame++) {
			int acq_fd = -1;
			if (!dmabuf_upload(c, nv, nv_staging, true, &acq_fd)) {
				v6ok = false;
				break;
			}
			XrWeaveSubmitSyncDXR sync = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_SYNC_DXR};
			sync.acquireFenceFd = acq_fd;
			XrWeaveDmabufDescDXR d = {(XrStructureType)XR_TYPE_WEAVE_DMABUF_DESC_DXR};
			fill_dmabuf_desc(d, nv, dup(nv.fd), 7);
			d.next = &sync;
			XrWeaveSubmitLayoutDXR lay = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_LAYOUT_DXR};
			lay.next = &d;
			lay.viewCount = 2;
			lay.tileColumns = 2;
			lay.tileRows = 1;
			lay.contentViewWidth = cvw;
			lay.contentViewHeight = cvh;
			XrWeaveSubmitInfoDXR s = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_INFO_DXR};
			s.next = &lay;
			s.firstChunk = XR_TRUE;
			XrWeaveOutputSyncDXR os = {(XrStructureType)XR_TYPE_WEAVE_OUTPUT_SYNC_DXR};
			XrWeaveOutputDmabufDXR od = {(XrStructureType)XR_TYPE_WEAVE_OUTPUT_DMABUF_DXR};
			od.next = &os;
			XrWeaveOutputDXR o = {(XrStructureType)XR_TYPE_WEAVE_OUTPUT_DXR};
			o.next = &od;
			XrResult r = pfn_submit(session, &s, &o);
			if (XR_FAILED(r)) {
				close(d.fd);
				close(acq_fd);
				LOG("v6 FAIL: xrWeaveSubmitDXR (dma-buf) frame %d -> %d", frame, (int)r);
				v6ok = false;
				break;
			}
			if (od.fd >= 0) {
				if (v6out.image == VK_NULL_HANDLE && import_dmabuf_output(vk, c.fn, od, v6out)) {
					LOG("v6: woven output %ux%u modifier 0x%016llx", od.width, od.height,
					    (unsigned long long)od.drmModifier);
				} else {
					close(od.fd);
				}
			}
			if (rel >= 0) {
				close(rel);
			}
			rel = os.releaseFenceFd; // wait only the last frame's
		}
		if (v6ok && v6out.image != VK_NULL_HANDLE && v6out.w == cvw && v6out.h == cvh) {
			Buffer v6rb;
			v6ok = create_buffer(vk, (VkDeviceSize)cvw * cvh * 4, v6rb) && dmabuf_readback(c, v6out, v6rb, rel);
			rel = -1;
			if (v6ok) {
				std::vector<uint8_t> v6px((const uint8_t *)v6rb.map, (const uint8_t *)v6rb.map + v6rb.size);
				dump_ppm(v6px, cvw, cvh, dump_dir + "/weave_probe_linux_dmabuf_v6_output.ppm");
				uint8_t r, g, b, a;
				sample(v6px, cvw, (int)cvw / 2, (int)cvh / 2, &r, &g, &b, &a);
				v6ok = r > 150 && g > 150 && b > 150;
				LOG("v6 centre @(%u,%u) = (%u,%u,%u) want white -> %s", cvw / 2, cvh / 2, r, g, b,
				    v6ok ? "OK" : "WRONG");
				destroy_buffer(vk, v6rb);
			}
		} else if (v6ok) {
			LOG("v6 FAIL: no %ux%u woven dma-buf", cvw, cvh);
			v6ok = false;
		}
		if (rel >= 0) {
			close(rel);
		}
		LOG("v6 N-view atlas (dma-buf): %s", v6ok ? "PASS" : "FAIL");
		pass = pass && v6ok;
		if (v6out.image != VK_NULL_HANDLE) {
			destroy_dmabuf_image(vk, v6out);
		}
		vkDeviceWaitIdle(vk.device);
		destroy_dmabuf_image(vk, nv);
		destroy_buffer(vk, nv_staging);
	}

	vkDeviceWaitIdle(vk.device);
	for (int i = 0; i < 2; i++) {
		destroy_dmabuf_image(vk, input[i]);
		destroy_buffer(vk, staging[i]);
	}
	destroy_dmabuf_image(vk, overlay);
	destroy_buffer(vk, ov_staging);
	destroy_buffer(vk, rb);
	vkDestroySemaphore(vk.device, c.acq_sem, nullptr);
	vkDestroySemaphore(vk.device, c.rel_sem, nullptr);
	vkDestroyFence(vk.device, c.up_fence, nullptr);
	vkDestroyFence(vk.device, c.rb_fence, nullptr);
	return pass;
}

int
main(int argc, char **argv)
{
	int frames = 600;
	long service_pid = 0;
	bool dmabuf = false, release_wait = true, force_linear = false;
	std::string dump_dir = "/tmp";
	if (const char *t = getenv("TMPDIR")) {
		dump_dir = t;
	}
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--frames=", 9) == 0) {
			frames = atoi(argv[i] + 9);
		} else if (strncmp(argv[i], "--service-pid=", 14) == 0) {
			service_pid = atol(argv[i] + 14);
		} else if (strncmp(argv[i], "--dump-dir=", 11) == 0) {
			dump_dir = argv[i] + 11;
		} else if (strcmp(argv[i], "--dmabuf") == 0) {
			dmabuf = true;
		} else if (strcmp(argv[i], "--no-release-wait") == 0) {
			release_wait = false; // negative control: must FAIL the pixel check
		} else if (strcmp(argv[i], "--linear") == 0) {
			force_linear = true; // LINEAR inputs instead of the driver's pick
		} else {
			LOG("usage: %s [--frames=N] [--service-pid=PID] [--dump-dir=DIR] [--dmabuf [--no-release-wait] "
			    "[--linear]]",
			    argv[0]);
			return 2;
		}
	}
	if (frames < 20) {
		frames = 20;
	}

	// ---- Instance with the weave + vulkan-enable extensions.
	uint32_t ext_count = 0;
	XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr));
	std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
	XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, exts.data()));
	bool has_weave = false, has_vk = false, has_rig = false;
	for (const auto &e : exts) {
		has_weave = has_weave || strcmp(e.extensionName, XR_DXR_WEAVE_EXTENSION_NAME) == 0;
		has_vk = has_vk || strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0;
		has_rig = has_rig || strcmp(e.extensionName, XR_DXR_VIEW_RIG_EXTENSION_NAME) == 0;
	}
	LOG("XR_DXR_weave:         %s", has_weave ? "AVAILABLE" : "NOT FOUND");
	LOG("XR_KHR_vulkan_enable: %s", has_vk ? "AVAILABLE" : "NOT FOUND");
	if (!has_weave || !has_vk) {
		return 1;
	}

	// XR_DXR_view_rig (optional): its XrViewDisplayRawDXR output routes
	// xrLocateViews through the server-side Kooima and reports the canvas the
	// service resolved — how the probe checks the weave-geometry window metrics.
	const char *enabled[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_DXR_WEAVE_EXTENSION_NAME,
	                         XR_DXR_VIEW_RIG_EXTENSION_NAME};
	XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
	snprintf(ici.applicationInfo.applicationName, sizeof(ici.applicationInfo.applicationName), "%s",
	         "DXRWeaveProbeLinux");
	ici.applicationInfo.applicationVersion = 1;
	snprintf(ici.applicationInfo.engineName, sizeof(ici.applicationInfo.engineName), "%s", "None");
	ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	ici.enabledExtensionCount = has_rig ? 3 : 2;
	ici.enabledExtensionNames = enabled;
	XrInstance instance = XR_NULL_HANDLE;
	XR_CHECK(xrCreateInstance(&ici, &instance));

	XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId system_id = XR_NULL_SYSTEM_ID;
	XR_CHECK(xrGetSystem(instance, &sgi, &system_id));

	// ---- Headless Vulkan device per XR_KHR_vulkan_enable.
	PFN_xrGetVulkanGraphicsRequirementsKHR pfn_req = nullptr;
	PFN_xrGetVulkanInstanceExtensionsKHR pfn_iext = nullptr;
	PFN_xrGetVulkanDeviceExtensionsKHR pfn_dext = nullptr;
	PFN_xrGetVulkanGraphicsDeviceKHR pfn_gdev = nullptr;
	xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&pfn_req);
	xrGetInstanceProcAddr(instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction *)&pfn_iext);
	xrGetInstanceProcAddr(instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction *)&pfn_dext);
	xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction *)&pfn_gdev);
	if (pfn_req == nullptr || pfn_iext == nullptr || pfn_dext == nullptr || pfn_gdev == nullptr) {
		LOG("failed to resolve XR_KHR_vulkan_enable entry points");
		return 1;
	}
	XrGraphicsRequirementsVulkanKHR vk_req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
	XR_CHECK(pfn_req(instance, system_id, &vk_req));

	uint32_t len = 0;
	pfn_iext(instance, system_id, 0, &len, nullptr);
	std::string iext_str(len, '\0');
	pfn_iext(instance, system_id, len, &len, iext_str.data());
	std::vector<std::string> iext_storage;
	split_exts(iext_str, iext_storage);
	std::vector<const char *> iexts;
	for (const auto &e : iext_storage) {
		iexts.push_back(e.c_str());
	}

	Vk vk;
	VkApplicationInfo app_info = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app_info.pApplicationName = "DXRWeaveProbeLinux";
	app_info.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo vici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	vici.pApplicationInfo = &app_info;
	vici.enabledExtensionCount = (uint32_t)iexts.size();
	vici.ppEnabledExtensionNames = iexts.data();
	if (vkCreateInstance(&vici, nullptr, &vk.instance) != VK_SUCCESS) {
		LOG("vkCreateInstance failed");
		return 1;
	}
	XR_CHECK(pfn_gdev(instance, system_id, vk.instance, &vk.phys));
	{
		VkPhysicalDeviceIDProperties idp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
		VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		p2.pNext = &idp;
		vkGetPhysicalDeviceProperties2(vk.phys, &p2);
		char uuid[33] = {0};
		for (int i = 0; i < 16; i++) {
			snprintf(uuid + i * 2, 3, "%02x", idp.deviceUUID[i]);
		}
		LOG("probe GPU: %s (deviceUUID %s) — stage A needs the service on the same device",
		    p2.properties.deviceName, uuid);
	}
	vkGetPhysicalDeviceMemoryProperties(vk.phys, &vk.mem_props);

	uint32_t qf_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &qf_count, nullptr);
	std::vector<VkQueueFamilyProperties> qfs(qf_count);
	vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &qf_count, qfs.data());
	vk.qfi = UINT32_MAX;
	for (uint32_t i = 0; i < qf_count; i++) {
		if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			vk.qfi = i;
			break;
		}
	}
	if (vk.qfi == UINT32_MAX) {
		LOG("no graphics queue family");
		return 1;
	}

	len = 0;
	pfn_dext(instance, system_id, 0, &len, nullptr);
	std::string dext_str(len, '\0');
	pfn_dext(instance, system_id, len, &len, dext_str.data());
	std::vector<std::string> dext_storage;
	split_exts(dext_str, dext_storage);
	// Stage A needs OPAQUE_FD export/import on the probe's device too; stage B
	// (--dmabuf) dma-buf + DRM modifiers, the foreign queue family and SYNC_FD
	// semaphores.
	std::vector<const char *> needs = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
	if (dmabuf) {
		for (const char *e : {VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
		                      VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
		                      VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME}) {
			needs.push_back(e);
		}
	}
	for (const char *need : needs) {
		bool have = false;
		for (const auto &e : dext_storage) {
			have = have || e == need;
		}
		if (!have) {
			dext_storage.push_back(need);
		}
	}
	std::vector<const char *> dexts;
	for (const auto &e : dext_storage) {
		dexts.push_back(e.c_str());
	}

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = vk.qfi;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = (uint32_t)dexts.size();
	dci.ppEnabledExtensionNames = dexts.data();
	if (vkCreateDevice(vk.phys, &dci, nullptr, &vk.device) != VK_SUCCESS) {
		LOG("vkCreateDevice failed");
		return 1;
	}
	vkGetDeviceQueue(vk.device, vk.qfi, 0, &vk.queue);
	vk.get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(vk.device, "vkGetMemoryFdKHR");
	if (vk.get_memory_fd == nullptr) {
		LOG("vkGetMemoryFdKHR unavailable");
		return 1;
	}
	{
		VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = vk.qfi;
		vkCreateCommandPool(vk.device, &pci, nullptr, &vk.pool);
		VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cai.commandPool = vk.pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		vkAllocateCommandBuffers(vk.device, &cai, &vk.cmd);
		VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		vkCreateFence(vk.device, &fci, nullptr, &vk.fence);
	}

	// ---- Forced-IPC session over the headless Vulkan device (no window binding).
	XrGraphicsBindingVulkanKHR vk_binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
	vk_binding.instance = vk.instance;
	vk_binding.physicalDevice = vk.phys;
	vk_binding.device = vk.device;
	vk_binding.queueFamilyIndex = vk.qfi;
	vk_binding.queueIndex = 0;
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &vk_binding;
	sci.systemId = system_id;
	XrSession session = XR_NULL_HANDLE;
	XR_CHECK(xrCreateSession(instance, &sci, &session));
	LOG("session created (IPC, headless Vulkan)");

	PFN_xrWeaveBindWindow2DXR pfn_bind2 = nullptr;
	PFN_xrWeaveSubmitDXR pfn_submit = nullptr;
	PFN_xrWeaveSnapWindowRectDXR pfn_snap = nullptr;
	xrGetInstanceProcAddr(instance, "xrWeaveBindWindow2DXR", (PFN_xrVoidFunction *)&pfn_bind2);
	xrGetInstanceProcAddr(instance, "xrWeaveSubmitDXR", (PFN_xrVoidFunction *)&pfn_submit);
	xrGetInstanceProcAddr(instance, "xrWeaveSnapWindowRectDXR", (PFN_xrVoidFunction *)&pfn_snap);
	if (pfn_bind2 == nullptr || pfn_submit == nullptr || pfn_snap == nullptr) {
		LOG("failed to resolve weave entry points");
		return 1;
	}

	// Bind a fake window id (recorded only) + the geometry stage A needs.
	XrWeaveWindowGeometryDXR geom = {(XrStructureType)XR_TYPE_WEAVE_WINDOW_GEOMETRY_DXR};
	geom.windowOriginOnScreen = kWinOrigin;
	geom.clientSize = {(int32_t)kWinW, (int32_t)kWinH};
	geom.displayId = -1;
	XrWeaveBindWindowInfoDXR bind = {(XrStructureType)XR_TYPE_WEAVE_BIND_WINDOW_INFO_DXR};
	bind.next = &geom;
	bind.windowHandle = (void *)(uintptr_t)0x1;
	XR_CHECK(pfn_bind2(session, &bind));
	LOG("bound fake window 0x1 at (%d,%d) %ux%u", kWinOrigin.x, kWinOrigin.y, kWinW, kWinH);

	if (dmabuf) {
		bool pass = run_dmabuf(vk, session, pfn_submit, frames, service_pid, release_wait, force_linear, dump_dir);
		xrDestroySession(session);
		xrDestroyInstance(instance);
		vkDestroyFence(vk.device, vk.fence, nullptr);
		vkDestroyCommandPool(vk.device, vk.pool, nullptr);
		vkDestroyDevice(vk.device, nullptr);
		vkDestroyInstance(vk.instance, nullptr);
		if (!release_wait) {
			// The negative control is EXPECTED to fail the pixel check.
			LOG("%s", pass ? "NEGATIVE CONTROL DID NOT FAIL (the race did not show)" : "FAIL (expected: negative control)");
			return pass ? 0 : 1;
		}
		LOG("%s", pass ? "PASS" : "FAIL");
		return pass ? 0 : 1;
	}

	// ---- Inputs: two identical window-sized squeezed-SBS images + the overlay.
	std::vector<uint8_t> in_px((size_t)kWinW * kWinH * 4);
	for (uint32_t y = 0; y < kWinH; y++) { // dark-gray background
		for (uint32_t x = 0; x < kWinW; x++) {
			put_px(in_px, kWinW, (int)x, (int)y, 32, 32, 32);
		}
	}
	fill_sbs_rect(in_px, kWinW, kRectA, /*left*/ 0xFF, /*right*/ 0x00); // -> RED after anaglyph
	fill_sbs_rect(in_px, kWinW, kRectB, /*left*/ 0x00, /*right*/ 0xFF); // -> CYAN after anaglyph

	std::vector<uint8_t> ov_px((size_t)kWinW * kWinH * 4, 0); // transparent premul field
	for (int y = kOverlayBar.offset.y; y < kOverlayBar.offset.y + kOverlayBar.extent.height; y++) {
		for (int x = kOverlayBar.offset.x; x < kOverlayBar.offset.x + kOverlayBar.extent.width; x++) {
			put_px(ov_px, kWinW, x, y, 230, 13, 230, 255); // opaque magenta (premul == straight)
		}
	}

	Image input[2], overlay;
	for (auto &img : input) {
		if (!create_exported_image(vk, kWinW, kWinH, img) || !upload_and_release(vk, img, in_px)) {
			LOG("FAIL: input image setup");
			return 1;
		}
	}
	if (!create_exported_image(vk, kWinW, kWinH, overlay) || !upload_and_release(vk, overlay, ov_px)) {
		LOG("FAIL: overlay image setup");
		return 1;
	}
	LOG("inputs exported as OPAQUE_FD fds %d/%d, overlay fd %d", input[0].fd, input[1].fd, overlay.fd);

	// ---- Batched submits (spec v3) + v5 firstChunk + v4 overlay.
	XrRect2Di rects[2] = {kRectA, kRectB};
	XrWeaveSubmitOverlaysDXR ov = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_OVERLAYS_DXR};
	ov.overlayTexture = (void *)(intptr_t)overlay.fd;
	ov.rectCount = 0; // whole-atlas composite (premul alpha authoritative)
	XrWeaveSubmitRectsDXR batch = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_RECTS_DXR};
	batch.next = &ov;
	batch.rectCount = 2;
	batch.rects = rects;
	XrWeaveSubmitInfoDXR submit = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_INFO_DXR};
	submit.next = &batch;
	submit.firstChunk = XR_TRUE;

	int out_fd = -1;
	uint32_t out_w = 0, out_h = 0;
	uint64_t last_fv = 0;
	int probe_fds_early = -1, svc_fds_early = -1;
	bool pass = true;
	for (int frame = 0; frame < frames; frame++) {
		// Alternate the two inputs: a rotating pool must hit the cache, not re-import.
		submit.inputTexture = (void *)(intptr_t)input[frame & 1].fd;
		XrWeaveOutputDXR out = {(XrStructureType)XR_TYPE_WEAVE_OUTPUT_DXR};
		XrResult r = pfn_submit(session, &submit, &out);
		if (XR_FAILED(r)) {
			LOG("FAIL: xrWeaveSubmitDXR frame %d -> %d", frame, (int)r);
			return 1;
		}
		if (out.weavedTexture != nullptr) {
			if (out_fd >= 0) {
				LOG("FAIL: a second output fd was handed back at frame %d without a resize", frame);
				pass = false;
				close((int)(intptr_t)out.weavedTexture);
			} else {
				out_fd = (int)(intptr_t)out.weavedTexture;
			}
		}
		if (out.fenceValue <= last_fv) {
			LOG("FAIL: fenceValue not monotonic (%llu after %llu)", (unsigned long long)out.fenceValue,
			    (unsigned long long)last_fv);
			pass = false;
		}
		last_fv = out.fenceValue;
		out_w = out.width;
		out_h = out.height;
		if (frame == 0) {
			LOG("frame 0: woven %ux%u, output fd=%d, fence=%p fenceValue=%llu, eyes=%u (valid=%d "
			    "tracking=%d) [%.3f,%.3f,%.3f]",
			    out.width, out.height, out_fd, out.fence, (unsigned long long)out.fenceValue, out.eyeCount,
			    (int)out.eyesValid, (int)out.eyesTracking, (double)out.eyes[0].x, (double)out.eyes[0].y,
			    (double)out.eyes[0].z);
		}
		if (frame == 10) {
			probe_fds_early = count_fds(0);
			svc_fds_early = service_pid > 0 ? count_fds(service_pid) : -1;
		}
	}
	int probe_fds_late = count_fds(0);
	int svc_fds_late = service_pid > 0 ? count_fds(service_pid) : -1;
	LOG("fd counts over %d frames: probe %d -> %d, service(pid %ld) %d -> %d", frames, probe_fds_early,
	    probe_fds_late, service_pid, svc_fds_early, svc_fds_late);
	if (probe_fds_late != probe_fds_early || svc_fds_late != svc_fds_early) {
		LOG("FAIL: fd count grew");
		pass = false;
	}

	if (out_fd < 0) {
		LOG("FAIL: no woven output fd was handed back");
		return 1;
	}
	if (out_w != kWinW || out_h != kWinH) {
		LOG("FAIL: output dims %ux%u != window dims %ux%u", out_w, out_h, kWinW, kWinH);
		return 1;
	}

	// ---- Import the woven output on OUR device and verify the anaglyph weave.
	Image output;
	if (!import_output_image(vk, out_fd, out_w, out_h, output)) {
		LOG("FAIL: could not import the woven output fd (OPAQUE_FD, same device)");
		return 1;
	}
	out_fd = -1; // consumed by the import
	std::vector<uint8_t> px;
	if (!readback(vk, output, px)) {
		LOG("FAIL: readback");
		return 1;
	}
	dump_ppm(px, out_w, out_h, dump_dir + "/weave_probe_linux_output.ppm");

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
	for (const auto &c : checks) {
		uint8_t r, g, b, a;
		sample(px, out_w, c.x, c.y, &r, &g, &b, &a);
		bool ok = c.want_red ? (r > 150 && g < 80 && b < 80) : (r < 80 && g > 150 && b > 150);
		LOG("%s @(%d,%d) = (%u,%u,%u,a=%u) -> %s", c.name, c.x, c.y, r, g, b, a, ok ? "OK" : "WRONG");
		pass = pass && ok;
	}
	{
		int bx = kOverlayBar.offset.x + kOverlayBar.extent.width / 2;
		int by = kOverlayBar.offset.y + kOverlayBar.extent.height / 2;
		uint8_t r, g, b, a;
		sample(px, out_w, bx, by, &r, &g, &b, &a);
		bool ok = (r > 150 && b > 150 && g < 80 && a > 200);
		LOG("v4 overlay bar @(%d,%d) = (%u,%u,%u,a=%u) want magenta+opaque -> %s", bx, by, r, g, b, a,
		    ok ? "OK" : "WRONG");
		pass = pass && ok;
	}
	{
		uint8_t r, g, b, a;
		sample(px, out_w, 10, 10, &r, &g, &b, &a);
		bool ok = (a < 40);
		LOG("v5 firstChunk gap @(10,10) alpha=%u want ~0 -> %s", a, ok ? "OK" : "WRONG");
		pass = pass && ok;
		int tx = kRectA.offset.x + kRectA.extent.width / 2;
		int ty = kRectA.offset.y + kRectA.extent.height / 2;
		sample(px, out_w, tx, ty, &r, &g, &b, &a);
		ok = (a > 200);
		LOG("v5 firstChunk tile @(%d,%d) alpha=%u want ~255 -> %s", tx, ty, a, ok ? "OK" : "WRONG");
		pass = pass && ok;
	}
	destroy_image(vk, output);

	// ---- Snap through the service's weave DP (identity on sim_display's
	// default period of 1 — the call must still succeed and return a rect).
	{
		XrRect2Di origin = {{kWinOrigin.x, kWinOrigin.y}, {(int32_t)kWinW, (int32_t)kWinH}};
		XrRect2Di target = {{kWinOrigin.x + 13, kWinOrigin.y + 7}, {(int32_t)kWinW, (int32_t)kWinH}};
		XrRect2Di snapped = {};
		XrResult r = pfn_snap(session, &origin, &target, &snapped);
		bool ok = XR_SUCCEEDED(r) && snapped.extent.width == target.extent.width;
		LOG("snap (%d,%d) -> (%d,%d) result=%d -> %s", target.offset.x, target.offset.y, snapped.offset.x,
		    snapped.offset.y, (int)r, ok ? "OK" : "WRONG");
		pass = pass && ok;
	}

	// ---- Server-side Kooima for the weave window (#1699): a locate chaining
	// XrViewDisplayRawDXR goes to the service (session_locate_views_rig ->
	// ipc_try_get_oop_view_poses), which now sources the canvas from the bound
	// weave geometry (multi_compositor_get_window_metrics). The raw canvas must
	// be the bound window: 1280x720 px, sim_display pitch 0.344 m / 1920 px.
	if (has_rig) {
		XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		rsci.poseInReferenceSpace.orientation.w = 1.0f;
		XrSpace local = XR_NULL_HANDLE;
		XR_CHECK(xrCreateReferenceSpace(session, &rsci, &local));
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
		vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		vli.displayTime = (XrTime)ts.tv_sec * 1000000000LL + ts.tv_nsec;
		vli.space = local;
		XrViewDisplayRawDXR raw = {(XrStructureType)XR_TYPE_VIEW_DISPLAY_RAW_DXR};
		XrViewState vs = {XR_TYPE_VIEW_STATE};
		vs.next = &raw;
		XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
		uint32_t n = 0;
		XrResult r = xrLocateViews(session, &vli, &vs, 2, &n, views);
		bool ok = XR_SUCCEEDED(r) && n == 2;
		for (uint32_t i = 0; ok && i < n; i++) {
			LOG("locate view %u: pos (%.3f,%.3f,%.3f) fov L=%.1f R=%.1f U=%.1f D=%.1f deg", i,
			    (double)views[i].pose.position.x, (double)views[i].pose.position.y,
			    (double)views[i].pose.position.z, (double)views[i].fov.angleLeft * 57.2958,
			    (double)views[i].fov.angleRight * 57.2958, (double)views[i].fov.angleUp * 57.2958,
			    (double)views[i].fov.angleDown * 57.2958);
		}
		const double want_w_m = 1280.0 * 0.344 / 1920.0, want_h_m = 720.0 * 0.344 / 1920.0;
		const bool canvas_ok = raw.canvasRectPx.extent.width == (int32_t)kWinW &&
		                       raw.canvasRectPx.extent.height == (int32_t)kWinH &&
		                       raw.canvasSizeMeters.width > want_w_m * 0.98 &&
		                       raw.canvasSizeMeters.width < want_w_m * 1.02 &&
		                       raw.canvasSizeMeters.height > want_h_m * 0.98 &&
		                       raw.canvasSizeMeters.height < want_h_m * 1.02;
		LOG("locate raw: canvas (%d,%d %dx%d)px %.4fx%.4fm eyes=%u tracking=%d (want %ux%u px, %.4fx%.4fm) -> %s",
		    raw.canvasRectPx.offset.x, raw.canvasRectPx.offset.y, raw.canvasRectPx.extent.width,
		    raw.canvasRectPx.extent.height, (double)raw.canvasSizeMeters.width,
		    (double)raw.canvasSizeMeters.height, raw.eyeCountOutput, (int)raw.isTracking, kWinW, kWinH, want_w_m,
		    want_h_m, canvas_ok ? "OK (per-window canvas)" : "WRONG");
		LOG("xrLocateViews -> %d (%u views)", (int)r, n);
		pass = pass && ok && canvas_ok;
		xrDestroySpace(local);
	} else {
		LOG("XR_DXR_view_rig not offered — server-side Kooima check skipped");
	}

	// ---- Spec-v6 N-view atlas (#774): 2x1 red|cyan, packed == input (stage A
	// supports the zero-copy case) -> one content view, anaglyph WHITE.
	{
		const uint32_t cvw = 640, cvh = 360;
		std::vector<uint8_t> nv_px((size_t)2 * cvw * cvh * 4);
		for (uint32_t y = 0; y < cvh; y++) {
			for (uint32_t x = 0; x < cvw; x++) {
				put_px(nv_px, 2 * cvw, (int)x, (int)y, 0xFF, 0x00, 0x00);         // tile0 left = RED
				put_px(nv_px, 2 * cvw, (int)(cvw + x), (int)y, 0x00, 0xFF, 0xFF); // tile1 right = CYAN
			}
		}
		Image nv_input;
		bool v6ok = create_exported_image(vk, 2 * cvw, cvh, nv_input) && upload_and_release(vk, nv_input, nv_px);
		XrWeaveSubmitLayoutDXR lay = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_LAYOUT_DXR};
		lay.viewCount = 2;
		lay.tileColumns = 2;
		lay.tileRows = 1;
		lay.contentViewWidth = cvw;
		lay.contentViewHeight = cvh;
		XrWeaveSubmitInfoDXR v6submit = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_INFO_DXR};
		v6submit.next = &lay;
		v6submit.inputTexture = (void *)(intptr_t)nv_input.fd;
		v6submit.firstChunk = XR_TRUE;
		int v6fd = -1;
		uint32_t v6w = 0, v6h = 0;
		for (int frame = 0; v6ok && frame < 3; frame++) {
			XrWeaveOutputDXR out = {(XrStructureType)XR_TYPE_WEAVE_OUTPUT_DXR};
			XrResult r = pfn_submit(session, &v6submit, &out);
			if (XR_FAILED(r)) {
				LOG("v6 FAIL: xrWeaveSubmitDXR frame %d -> %d", frame, (int)r);
				v6ok = false;
				break;
			}
			if (out.weavedTexture != nullptr && v6fd < 0) {
				v6fd = (int)(intptr_t)out.weavedTexture;
			}
			v6w = out.width;
			v6h = out.height;
		}
		if (v6ok && (v6fd < 0 || v6w != cvw || v6h != cvh)) {
			LOG("v6 FAIL: output fd=%d dims %ux%u (want %ux%u)", v6fd, v6w, v6h, cvw, cvh);
			v6ok = false;
		}
		Image v6out;
		if (v6ok && import_output_image(vk, v6fd, v6w, v6h, v6out)) {
			v6fd = -1;
			std::vector<uint8_t> v6px;
			if (readback(vk, v6out, v6px)) {
				dump_ppm(v6px, v6w, v6h, dump_dir + "/weave_probe_linux_v6_output.ppm");
				uint8_t r, g, b, a;
				sample(v6px, v6w, (int)cvw / 2, (int)cvh / 2, &r, &g, &b, &a);
				bool white = (r > 150 && g > 150 && b > 150);
				LOG("v6 centre @(%u,%u) = (%u,%u,%u) want white -> %s", cvw / 2, cvh / 2, r, g, b,
				    white ? "OK" : "WRONG");
				v6ok = white;
			} else {
				v6ok = false;
			}
			destroy_image(vk, v6out);
		} else if (v6ok) {
			LOG("v6 FAIL: could not import the v6 output");
			v6ok = false;
		}
		if (v6fd >= 0) {
			close(v6fd);
		}
		LOG("v6 N-view atlas: %s", v6ok ? "PASS" : "FAIL");
		pass = pass && v6ok;
		destroy_image(vk, nv_input);
	}

	xrDestroySession(session);
	xrDestroyInstance(instance);
	destroy_image(vk, input[0]);
	destroy_image(vk, input[1]);
	destroy_image(vk, overlay);
	vkDestroyFence(vk.device, vk.fence, nullptr);
	vkDestroyCommandPool(vk.device, vk.pool, nullptr);
	vkDestroyDevice(vk.device, nullptr);
	vkDestroyInstance(vk.instance, nullptr);

	LOG("%s", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
