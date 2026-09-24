// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Headless tests for the desktop-Linux dma-buf / sync_file helpers
 *         (vk/vk_dmabuf.h, #1699 R5).
 *
 * Runs against every Vulkan physical device on the box (typically the GPU and
 * lavapipe). A device that does not offer dma-buf + DRM format modifiers is
 * skipped with a WARN; if none does (a CI runner whose lavapipe has no DRM
 * device), every case SKIPs rather than fails.
 *
 * What is pinned:
 *  (a) export -> same-process re-import through the descriptor -> a known
 *      pattern survives the round trip, bit-exact, for LINEAR and for the
 *      modifier the driver picks from everything it can export, in both
 *      byte orders (ABGR8888 / ARGB8888), across a FOREIGN release/acquire and
 *      a SYNC_FD semaphore hop;
 *  (b) SYNC_FD semaphore export -> import -> GPU wait;
 *  (c) fd hygiene: 200 export/import/destroy cycles leave /proc/self/fd flat;
 *  (d) every failure path of the importers closes the fd it was given.
 */

#include "catch_amalgamated.hpp"

#include <xrt/xrt_config_os.h>
#include <xrt/xrt_config_have.h>

#if defined(XRT_OS_LINUX_DESKTOP) && defined(XRT_HAVE_VULKAN)

#include "vktest_init_bundle.hpp"

#include <vk/vk_dmabuf.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>


namespace {

/*
 * The service device's list (null_compositor.c) for the dma-buf path, on top
 * of vktest_init_bundle.hpp's required set.
 */
const char *dmabuf_optional_device_extensions[] = {
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,        //
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,     //
    VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,         //
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,         //
    VK_KHR_MAINTENANCE_1_EXTENSION_NAME,             //
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,             //
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,  //
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,   //
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, //
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,      //
};

bool
init_bundle_on_gpu(struct vk_bundle *vk, int gpu_index)
{
	unique_string_list required_instance{
	    u_string_list_create_from_array(instance_extensions_common, ARRAY_SIZE(instance_extensions_common))};
	unique_string_list optional_instance{u_string_list_create()};
	unique_string_list required_device{
	    u_string_list_create_from_array(required_device_extensions, ARRAY_SIZE(required_device_extensions))};
	unique_string_list optional_device{u_string_list_create_from_array(
	    dmabuf_optional_device_extensions, ARRAY_SIZE(dmabuf_optional_device_extensions))};

	U_ZERO(vk);
	comp_vulkan_arguments args{};
	args.required_instance_version = VK_MAKE_VERSION(1, 0, 0);
	args.get_instance_proc_address = vkGetInstanceProcAddr;
	args.required_instance_extensions = required_instance.get();
	args.optional_instance_extensions = optional_instance.get();
	args.required_device_extensions = required_device.get();
	args.optional_device_extensions = optional_device.get();
	args.log_level = U_LOGGING_WARN;
	args.only_compute_queue = false;
	args.timeline_semaphore = true;
	args.selected_gpu_index = gpu_index;
	args.client_gpu_index = gpu_index;
	comp_vulkan_results results{};
	return comp_vulkan_init_bundle(vk, &args, &results);
}

std::string
device_name(struct vk_bundle *vk)
{
	VkPhysicalDeviceProperties props{};
	vk->vkGetPhysicalDeviceProperties(vk->physical_device, &props);
	return props.deviceName;
}

/*!
 * Run @p body once per physical device that supports the dma-buf path.
 * Returns how many devices ran it. Stops at the first index the loader does
 * not have.
 */
int
for_each_dmabuf_device(const std::function<void(struct vk_bundle *)> &body, bool need_sync_fd = false)
{
	int ran = 0;
	for (int gpu = 0; gpu < 8; gpu++) {
		unique_vk_bundle vk = makeVkBundle();
		if (!init_bundle_on_gpu(vk.get(), gpu)) {
			// Index past the end (or a device that cannot even start):
			// nothing to own, the bundle was zeroed on failure.
			vk.release();
			break;
		}
		const std::string name = device_name(vk.get());
		if (!vk_dmabuf_supported(vk.get())) {
			WARN("GPU " << gpu << " (" << name << "): no dma-buf + DRM format modifier support, skipped");
			continue;
		}
		if (need_sync_fd && !vk_dmabuf_sync_fd_supported(vk.get())) {
			WARN("GPU " << gpu << " (" << name << "): no SYNC_FD binary semaphores, skipped");
			continue;
		}
		INFO("GPU " << gpu << ": " << name);
		body(vk.get());
		ran++;
	}
	return ran;
}

int
count_open_fds()
{
	int n = 0;
	DIR *dir = opendir("/proc/self/fd");
	if (dir == nullptr) {
		return -1;
	}
	while (struct dirent *e = readdir(dir)) {
		if (e->d_name[0] != '.') {
			n++;
		}
	}
	closedir(dir);
	return n;
}

/*!
 * The fd count once it has stopped moving. A software driver may close a
 * sync_file from a worker thread a moment after the wait returns, so a single
 * sample can catch one in flight, at either end of the measurement.
 */
int
count_open_fds_settled(struct vk_bundle *vk)
{
	vk->vkDeviceWaitIdle(vk->device);
	int last = count_open_fds();
	for (int i = 0; i < 100; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		const int now = count_open_fds();
		if (now == last) {
			return now;
		}
		last = now;
	}
	return last;
}

bool
fd_is_closed(int fd)
{
	return fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

//! Deterministic RGBA-ish byte pattern, 4 bytes per texel, tightly packed.
std::vector<uint8_t>
make_pattern(uint32_t w, uint32_t h, uint32_t seed)
{
	std::vector<uint8_t> px((size_t)w * h * 4);
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			uint8_t *p = &px[((size_t)y * w + x) * 4];
			p[0] = (uint8_t)(x + seed);
			p[1] = (uint8_t)(y * 3 + seed);
			p[2] = (uint8_t)((x ^ y) + 7 * seed);
			p[3] = (uint8_t)(x * y + 11);
		}
	}
	return px;
}

//! A host-visible, coherent buffer.
struct HostBuffer
{
	struct vk_bundle *vk = nullptr;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	uint8_t *map = nullptr;

	HostBuffer(struct vk_bundle *vk_, VkDeviceSize size) : vk(vk_)
	{
		VkBufferCreateInfo bci{};
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = size;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		REQUIRE(vk->vkCreateBuffer(vk->device, &bci, nullptr, &buffer) == VK_SUCCESS);
		VkMemoryRequirements req{};
		vk->vkGetBufferMemoryRequirements(vk->device, buffer, &req);
		uint32_t type = 0;
		REQUIRE(vk_get_memory_type(vk, req.memoryTypeBits,
		                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		                           &type));
		VkMemoryAllocateInfo mai{};
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = type;
		REQUIRE(vk->vkAllocateMemory(vk->device, &mai, nullptr, &memory) == VK_SUCCESS);
		REQUIRE(vk->vkBindBufferMemory(vk->device, buffer, memory, 0) == VK_SUCCESS);
		void *p = nullptr;
		REQUIRE(vk->vkMapMemory(vk->device, memory, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS);
		map = static_cast<uint8_t *>(p);
	}
	~HostBuffer()
	{
		if (memory != VK_NULL_HANDLE) {
			vk->vkUnmapMemory(vk->device, memory);
			vk->vkFreeMemory(vk->device, memory, nullptr);
		}
		if (buffer != VK_NULL_HANDLE) {
			vk->vkDestroyBuffer(vk->device, buffer, nullptr);
		}
	}
};

//! One command buffer + fence on the main queue, recorded and submitted synchronously.
struct OneShot
{
	struct vk_bundle *vk;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;

	explicit OneShot(struct vk_bundle *vk_) : vk(vk_)
	{
		VkCommandPoolCreateInfo cpi{};
		cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		cpi.queueFamilyIndex = vk->main_queue->family_index;
		REQUIRE(vk->vkCreateCommandPool(vk->device, &cpi, nullptr, &pool) == VK_SUCCESS);
		VkCommandBufferAllocateInfo cai{};
		cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cai.commandPool = pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		REQUIRE(vk->vkAllocateCommandBuffers(vk->device, &cai, &cmd) == VK_SUCCESS);
		VkFenceCreateInfo fci{};
		fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		REQUIRE(vk->vkCreateFence(vk->device, &fci, nullptr, &fence) == VK_SUCCESS);
	}
	~OneShot()
	{
		vk->vkDestroyFence(vk->device, fence, nullptr);
		vk->vkDestroyCommandPool(vk->device, pool, nullptr);
	}

	VkCommandBuffer
	begin()
	{
		REQUIRE(vk->vkResetCommandBuffer(cmd, 0) == VK_SUCCESS);
		VkCommandBufferBeginInfo bi{};
		bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		REQUIRE(vk->vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS);
		return cmd;
	}

	//! End, submit (optionally waiting / signalling one semaphore), wait for completion.
	void
	submit_and_wait(VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE)
	{
		REQUIRE(vk->vkEndCommandBuffer(cmd) == VK_SUCCESS);
		VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkSubmitInfo si{};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.waitSemaphoreCount = wait != VK_NULL_HANDLE ? 1 : 0;
		si.pWaitSemaphores = &wait;
		si.pWaitDstStageMask = &wait_stage;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cmd;
		si.signalSemaphoreCount = signal != VK_NULL_HANDLE ? 1 : 0;
		si.pSignalSemaphores = &signal;
		REQUIRE(vk->vkQueueSubmit(vk->main_queue->queue, 1, &si, fence) == VK_SUCCESS);
		REQUIRE(vk->vkWaitForFences(vk->device, 1, &fence, VK_TRUE, UINT64_C(5000000000)) == VK_SUCCESS);
		REQUIRE(vk->vkResetFences(vk->device, 1, &fence) == VK_SUCCESS);
	}
};

VkImageMemoryBarrier
plain_barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst)
{
	VkImageMemoryBarrier b{};
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.srcAccessMask = src;
	b.dstAccessMask = dst;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	return b;
}

VkBufferImageCopy
full_copy(uint32_t w, uint32_t h)
{
	VkBufferImageCopy c{};
	c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	c.imageExtent = {w, h, 1};
	return c;
}

/*!
 * Export an image, fill it with a pattern, release it to FOREIGN signalling a
 * SYNC_FD semaphore; re-import through the descriptor, acquire from FOREIGN
 * behind the imported sync_file, read it back, compare.
 */
void
roundtrip(struct vk_bundle *vk, VkFormat format, const uint64_t *mods, uint32_t mod_count, const char *what)
{
	const uint32_t w = 257, h = 131; // odd sizes: exercises stride padding
	const VkImageUsageFlags export_usage =
	    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	const bool sync_fd = vk_dmabuf_sync_fd_supported(vk);

	struct xrt_weave_dmabuf_output_desc out{};
	VkImage src_image = VK_NULL_HANDLE;
	VkDeviceMemory src_memory = VK_NULL_HANDLE;
	VkResult ret = vk_create_exportable_dmabuf_image(vk, w, h, format, export_usage, mods, mod_count, &out,
	                                                 &src_image, &src_memory);
	INFO(what << ": vk_create_exportable_dmabuf_image -> " << vk_result_string(ret));
	REQUIRE(ret == VK_SUCCESS);
	REQUIRE(out.fd >= 0);
	CHECK(out.width == w);
	CHECK(out.height == h);
	CHECK(out.plane_count >= 1);
	CHECK(out.strides[0] >= w * 4);
	CHECK(out.size > 0);
	bool has_alpha = false;
	CHECK(vk_dmabuf_fourcc_to_vk_format(out.drm_fourcc, &has_alpha) == format);
	CHECK(has_alpha);
	if (mods == nullptr) {
		CHECK(out.drm_modifier == DRM_FORMAT_MOD_LINEAR);
	}
	char line[160];
	snprintf(line, sizeof(line), "%s: fourcc 0x%08x modifier 0x%016" PRIx64 " planes %u stride %u size %" PRIu64,
	         what, out.drm_fourcc, out.drm_modifier, out.plane_count, out.strides[0], out.size);
	WARN(device_name(vk) << ": " << line);

	const std::vector<uint8_t> pattern = make_pattern(w, h, (uint32_t)out.drm_modifier ^ (uint32_t)format);
	HostBuffer upload(vk, pattern.size());
	HostBuffer readback(vk, pattern.size());
	memcpy(upload.map, pattern.data(), pattern.size());
	memset(readback.map, 0xcd, pattern.size());

	OneShot one(vk);
	VkSemaphore release_sem = VK_NULL_HANDLE;
	if (sync_fd) {
		REQUIRE(vk_create_exportable_sync_fd_semaphore(vk, &release_sem) == VK_SUCCESS);
	}

	// Producer: fill, hand over to FOREIGN.
	VkCommandBuffer cmd = one.begin();
	VkImageMemoryBarrier to_dst =
	    plain_barrier(src_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
	                  VK_ACCESS_TRANSFER_WRITE_BIT);
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
	                         0, nullptr, 1, &to_dst);
	VkBufferImageCopy region = full_copy(w, h);
	vk->vkCmdCopyBufferToImage(cmd, upload.buffer, src_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	vk_dmabuf_cmd_release_foreign(vk, cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	one.submit_and_wait(VK_NULL_HANDLE, release_sem);

	int fence_fd = -1;
	if (sync_fd) {
		REQUIRE(vk_semaphore_export_sync_fd(vk, release_sem, &fence_fd) == VK_SUCCESS);
	}

	// Consumer: import through the descriptor, exactly as the wire delivers it.
	struct xrt_weave_dmabuf_desc in{};
	in.fd = dup(out.fd);
	REQUIRE(in.fd >= 0);
	in.width = out.width;
	in.height = out.height;
	in.drm_fourcc = out.drm_fourcc;
	in.drm_modifier = out.drm_modifier;
	in.plane_count = out.plane_count;
	for (uint32_t i = 0; i < out.plane_count; i++) {
		in.offsets[i] = out.offsets[i];
		in.strides[i] = out.strides[i];
	}
	VkImage dst_image = VK_NULL_HANDLE;
	VkDeviceMemory dst_memory = VK_NULL_HANDLE;
	VkFormat got_format = VK_FORMAT_UNDEFINED;
	ret =
	    vk_create_image_from_dmabuf(vk, &in, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &dst_image, &dst_memory, &got_format);
	INFO(what << ": vk_create_image_from_dmabuf -> " << vk_result_string(ret));
	REQUIRE(ret == VK_SUCCESS);
	CHECK(got_format == format);

	VkSemaphore acquire_sem = VK_NULL_HANDLE;
	if (sync_fd) {
		VkSemaphoreCreateInfo sci{};
		sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		REQUIRE(vk->vkCreateSemaphore(vk->device, &sci, nullptr, &acquire_sem) == VK_SUCCESS);
		REQUIRE(vk_semaphore_import_sync_fd(vk, acquire_sem, fence_fd) == VK_SUCCESS);
		fence_fd = -1;
	}

	cmd = one.begin();
	vk_dmabuf_cmd_acquire_foreign(vk, cmd, dst_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	vk->vkCmdCopyImageToBuffer(cmd, dst_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
	vk_dmabuf_cmd_release_foreign(vk, cmd, dst_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	one.submit_and_wait(acquire_sem, VK_NULL_HANDLE);

	size_t mismatches = 0;
	for (size_t i = 0; i < pattern.size(); i++) {
		mismatches += readback.map[i] != pattern[i];
	}
	INFO(what << ": " << mismatches << " of " << pattern.size() << " bytes differ");
	CHECK(mismatches == 0);

	if (acquire_sem != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, acquire_sem, nullptr);
	}
	if (release_sem != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, release_sem, nullptr);
	}
	vk->vkDestroyImage(vk->device, dst_image, nullptr);
	vk->vkFreeMemory(vk->device, dst_memory, nullptr);
	vk->vkDestroyImage(vk->device, src_image, nullptr);
	vk->vkFreeMemory(vk->device, src_memory, nullptr);
	close(out.fd);
}

//! Everything the device can export for @p format with the round-trip's usage.
std::vector<uint64_t>
exportable_modifiers(struct vk_bundle *vk, VkFormat format)
{
	const VkImageUsageFlags usage =
	    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	struct vk_dmabuf_modifier_info infos[64];
	uint32_t count = 0;
	VkResult ret = vk_dmabuf_query_modifiers(vk, format, usage, infos, 64, &count);
	REQUIRE((ret == VK_SUCCESS || ret == VK_INCOMPLETE));
	std::vector<uint64_t> mods;
	for (uint32_t i = 0; i < count && i < 64; i++) {
		char line[128];
		snprintf(line, sizeof(line), "format %d modifier 0x%016" PRIx64 " planes %u import %d export %d",
		         format, infos[i].modifier, infos[i].plane_count, infos[i].importable, infos[i].exportable);
		UNSCOPED_INFO(line);
		if (infos[i].exportable && infos[i].importable) {
			mods.push_back(infos[i].modifier);
		}
	}
	return mods;
}

//! A real dma-buf fd to feed the failure paths (the caller closes it).
int
make_small_dmabuf(struct vk_bundle *vk, struct xrt_weave_dmabuf_output_desc *out)
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	REQUIRE(vk_create_exportable_dmabuf_image(vk, 64, 64, VK_FORMAT_R8G8B8A8_UNORM,
	                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	                                          nullptr, 0, out, &image, &memory) == VK_SUCCESS);
	// The fd keeps the dma-buf alive on its own.
	vk->vkDestroyImage(vk->device, image, nullptr);
	vk->vkFreeMemory(vk->device, memory, nullptr);
	return out->fd;
}

struct xrt_weave_dmabuf_desc
desc_from(const struct xrt_weave_dmabuf_output_desc &out, int fd)
{
	struct xrt_weave_dmabuf_desc in{};
	in.fd = fd;
	in.width = out.width;
	in.height = out.height;
	in.drm_fourcc = out.drm_fourcc;
	in.drm_modifier = out.drm_modifier;
	in.plane_count = out.plane_count;
	for (uint32_t i = 0; i < out.plane_count; i++) {
		in.offsets[i] = out.offsets[i];
		in.strides[i] = out.strides[i];
	}
	return in;
}

} // namespace


TEST_CASE("vk_dmabuf fourcc map", "[vk_dmabuf]")
{
	bool a = false;
	CHECK(vk_dmabuf_fourcc_to_vk_format(DRM_FORMAT_ABGR8888, &a) == VK_FORMAT_R8G8B8A8_UNORM);
	CHECK(a);
	CHECK(vk_dmabuf_fourcc_to_vk_format(DRM_FORMAT_XBGR8888, &a) == VK_FORMAT_R8G8B8A8_UNORM);
	CHECK_FALSE(a);
	CHECK(vk_dmabuf_fourcc_to_vk_format(DRM_FORMAT_ARGB8888, &a) == VK_FORMAT_B8G8R8A8_UNORM);
	CHECK(a);
	CHECK(vk_dmabuf_fourcc_to_vk_format(DRM_FORMAT_XRGB8888, &a) == VK_FORMAT_B8G8R8A8_UNORM);
	CHECK_FALSE(a);
	CHECK(vk_dmabuf_fourcc_to_vk_format(0x3231564e /* NV12 */, nullptr) == VK_FORMAT_UNDEFINED);

	CHECK(vk_dmabuf_vk_format_to_fourcc(VK_FORMAT_R8G8B8A8_UNORM, true) == DRM_FORMAT_ABGR8888);
	CHECK(vk_dmabuf_vk_format_to_fourcc(VK_FORMAT_R8G8B8A8_UNORM, false) == DRM_FORMAT_XBGR8888);
	CHECK(vk_dmabuf_vk_format_to_fourcc(VK_FORMAT_B8G8R8A8_UNORM, true) == DRM_FORMAT_ARGB8888);
	CHECK(vk_dmabuf_vk_format_to_fourcc(VK_FORMAT_B8G8R8A8_UNORM, false) == DRM_FORMAT_XRGB8888);
	CHECK(vk_dmabuf_vk_format_to_fourcc(VK_FORMAT_R8G8B8A8_SRGB, true) == 0);
	// The fourcc values are kernel ABI.
	CHECK(DRM_FORMAT_ABGR8888 == 0x34324241u);
	CHECK(DRM_FORMAT_ARGB8888 == 0x34325241u);
	CHECK(DRM_FORMAT_MOD_LINEAR == 0u);
	CHECK(DRM_FORMAT_MOD_INVALID == UINT64_C(0x00ffffffffffffff));
}

TEST_CASE("vk_dmabuf export then re-import round-trips a pattern", "[vk_dmabuf]")
{
	int ran = for_each_dmabuf_device([](struct vk_bundle *vk) {
		for (VkFormat format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}) {
			const std::string fmt = format == VK_FORMAT_R8G8B8A8_UNORM ? "ABGR8888" : "ARGB8888";
			roundtrip(vk, format, nullptr, 0, (fmt + " LINEAR (NULL list)").c_str());

			std::vector<uint64_t> mods = exportable_modifiers(vk, format);
			if (mods.empty()) {
				WARN(fmt << ": no modifier both importable and exportable; driver-pick case skipped");
				continue;
			}
			roundtrip(vk, format, mods.data(), (uint32_t)mods.size(), (fmt + " driver pick").c_str());

			// INVALID in the list (a Wayland feedback list carries it) is dropped, not fatal.
			mods.push_back(DRM_FORMAT_MOD_INVALID);
			roundtrip(vk, format, mods.data(), (uint32_t)mods.size(),
			          (fmt + " driver pick, list with INVALID").c_str());
		}
	});
	if (ran == 0) {
		SKIP("no Vulkan device with dma-buf + DRM format modifier support");
	}
}

TEST_CASE("vk_dmabuf SYNC_FD semaphore export then import", "[vk_dmabuf]")
{
	int ran = for_each_dmabuf_device(
	    [](struct vk_bundle *vk) {
		    OneShot one(vk);
		    VkSemaphore exported = VK_NULL_HANDLE;
		    REQUIRE(vk_create_exportable_sync_fd_semaphore(vk, &exported) == VK_SUCCESS);

		    VkSemaphoreCreateInfo sci{};
		    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		    VkSemaphore imported = VK_NULL_HANDLE;
		    REQUIRE(vk->vkCreateSemaphore(vk->device, &sci, nullptr, &imported) == VK_SUCCESS);

		    for (int round = 0; round < 3; round++) { // the exported semaphore is reusable
			    one.begin();
			    one.submit_and_wait(VK_NULL_HANDLE, exported);
			    int fd = -1;
			    REQUIRE(vk_semaphore_export_sync_fd(vk, exported, &fd) == VK_SUCCESS);
			    // -1 is a legal "already signalled" sync_file; otherwise it must be a live fd.
			    if (fd >= 0) {
				    CHECK(fcntl(fd, F_GETFD) != -1);
			    }
			    REQUIRE(vk_semaphore_import_sync_fd(vk, imported, fd) == VK_SUCCESS);
			    one.begin();
			    one.submit_and_wait(imported, VK_NULL_HANDLE); // GPU wait on the import
		    }

		    // -1 import: already signalled, nothing to close.
		    REQUIRE(vk_semaphore_import_sync_fd(vk, imported, -1) == VK_SUCCESS);
		    one.begin();
		    one.submit_and_wait(imported, VK_NULL_HANDLE);

		    vk->vkDestroySemaphore(vk->device, imported, nullptr);
		    vk->vkDestroySemaphore(vk->device, exported, nullptr);
	    },
	    true);
	if (ran == 0) {
		SKIP("no Vulkan device with dma-buf + SYNC_FD semaphore support");
	}
}

TEST_CASE("vk_dmabuf fd table stays flat over 200 cycles", "[vk_dmabuf]")
{
	int ran = for_each_dmabuf_device([](struct vk_bundle *vk) {
		const bool sync_fd = vk_dmabuf_sync_fd_supported(vk);
		OneShot one(vk);
		VkSemaphore exported = VK_NULL_HANDLE;
		VkSemaphore imported = VK_NULL_HANDLE;
		if (sync_fd) {
			REQUIRE(vk_create_exportable_sync_fd_semaphore(vk, &exported) == VK_SUCCESS);
			VkSemaphoreCreateInfo sci{};
			sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			REQUIRE(vk->vkCreateSemaphore(vk->device, &sci, nullptr, &imported) == VK_SUCCESS);
		}

		auto cycle = [&]() {
			struct xrt_weave_dmabuf_output_desc out{};
			VkImage ex_image = VK_NULL_HANDLE, im_image = VK_NULL_HANDLE;
			VkDeviceMemory ex_mem = VK_NULL_HANDLE, im_mem = VK_NULL_HANDLE;
			REQUIRE(vk_create_exportable_dmabuf_image(vk, 64, 64, VK_FORMAT_B8G8R8A8_UNORM,
			                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
			                                              VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			                                          nullptr, 0, &out, &ex_image, &ex_mem) == VK_SUCCESS);
			struct xrt_weave_dmabuf_desc in = desc_from(out, dup(out.fd));
			REQUIRE(vk_create_image_from_dmabuf(vk, &in, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &im_image,
			                                    &im_mem, nullptr) == VK_SUCCESS);
			if (sync_fd) {
				one.begin();
				one.submit_and_wait(VK_NULL_HANDLE, exported);
				int fd = -1;
				REQUIRE(vk_semaphore_export_sync_fd(vk, exported, &fd) == VK_SUCCESS);
				REQUIRE(vk_semaphore_import_sync_fd(vk, imported, fd) == VK_SUCCESS);
				one.begin();
				one.submit_and_wait(imported, VK_NULL_HANDLE);
			}
			vk->vkDestroyImage(vk->device, im_image, nullptr);
			vk->vkFreeMemory(vk->device, im_mem, nullptr);
			vk->vkDestroyImage(vk->device, ex_image, nullptr);
			vk->vkFreeMemory(vk->device, ex_mem, nullptr);
			close(out.fd);
		};

		cycle(); // warm-up: lets a driver open whatever it opens lazily once
		const int before = count_open_fds_settled(vk);
		REQUIRE(before > 0);
		int peak = before;
		for (int i = 0; i < 200; i++) {
			cycle();
			const int now = count_open_fds();
			peak = now > peak ? now : peak;
		}
		// The peak is sampled unsettled, so it may see a sync_file in flight;
		// a leak of one fd per cycle would put it ~200 above the start.
		const int after = count_open_fds_settled(vk);
		WARN(device_name(vk) << ": fds before " << before << " peak " << peak << " after " << after);
		CHECK(after == before);
		CHECK(peak <= before + 2);

		if (sync_fd) {
			vk->vkDestroySemaphore(vk->device, imported, nullptr);
			vk->vkDestroySemaphore(vk->device, exported, nullptr);
		}
	});
	if (ran == 0) {
		SKIP("no Vulkan device with dma-buf + DRM format modifier support");
	}
}

TEST_CASE("vk_dmabuf failure paths close the fd", "[vk_dmabuf]")
{
	int ran = for_each_dmabuf_device([](struct vk_bundle *vk) {
		struct xrt_weave_dmabuf_output_desc out{};
		const int keep = make_small_dmabuf(vk, &out);
		REQUIRE(keep >= 0);

		auto expect_closed = [&](struct xrt_weave_dmabuf_desc in, const char *what) {
			const int fd = in.fd;
			REQUIRE(fd >= 0);
			VkImage image = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			VkResult ret = vk_create_image_from_dmabuf(vk, &in, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &image,
			                                           &memory, nullptr);
			INFO(what << " -> " << vk_result_string(ret));
			CHECK(ret != VK_SUCCESS);
			CHECK(image == VK_NULL_HANDLE);
			CHECK(memory == VK_NULL_HANDLE);
			CHECK(fd_is_closed(fd));
		};

		struct xrt_weave_dmabuf_desc in = desc_from(out, dup(keep));
		in.drm_modifier = UINT64_C(0x0123456789abcdef); // no vendor offers this
		expect_closed(in, "bogus modifier");

		in = desc_from(out, dup(keep));
		in.drm_modifier = DRM_FORMAT_MOD_INVALID;
		expect_closed(in, "DRM_FORMAT_MOD_INVALID");

		in = desc_from(out, dup(keep));
		in.drm_fourcc = 0x3231564e; // NV12: not mapped
		expect_closed(in, "unmapped fourcc");

		in = desc_from(out, dup(keep));
		in.plane_count = out.plane_count + 1;
		expect_closed(in, "wrong plane count");

		in = desc_from(out, dup(keep));
		in.width = 4096;
		in.height = 4096;
		in.strides[0] = 4096 * 4;
		expect_closed(in, "descriptor larger than the dma-buf");

		int pipe_fds[2];
		REQUIRE(pipe(pipe_fds) == 0);
		close(pipe_fds[1]);
		in = desc_from(out, pipe_fds[0]);
		expect_closed(in, "not a dma-buf");

		// The originally exported fd was never touched by any of that.
		CHECK_FALSE(fd_is_closed(keep));
		close(keep);

		if (vk_dmabuf_sync_fd_supported(vk)) {
			VkSemaphoreCreateInfo sci{};
			sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			VkSemaphore sem = VK_NULL_HANDLE;
			REQUIRE(vk->vkCreateSemaphore(vk->device, &sci, nullptr, &sem) == VK_SUCCESS);
			// A pipe is not a sync_file: the import must fail and still close it.
			REQUIRE(pipe(pipe_fds) == 0);
			close(pipe_fds[1]);
			VkResult ret = vk_semaphore_import_sync_fd(vk, sem, pipe_fds[0]);
			INFO("semaphore import of a pipe -> " << vk_result_string(ret));
			if (ret == VK_SUCCESS) {
				// Some drivers (lavapipe) take any pollable fd. Then Vulkan
				// owns it and may keep it open, so there is nothing to pin.
				WARN(device_name(vk)
				     << " accepted a pipe as a SYNC_FD; failure path not reachable here");
			} else {
				CHECK(fd_is_closed(pipe_fds[0]));
			}
			vk->vkDestroySemaphore(vk->device, sem, nullptr);
		}
	});
	if (ran == 0) {
		SKIP("no Vulkan device with dma-buf + DRM format modifier support");
	}
}

TEST_CASE("vk_dmabuf importer closes the fd when the device lacks support", "[vk_dmabuf]")
{
	// Holds on any device, supported or not: an unsupported device must still
	// consume the fd. Use a pipe so no dma-buf is needed.
	unique_vk_bundle vk = makeVkBundle();
	if (!vktest_init_bundle(vk.get())) {
		vk.release();
		SKIP("no Vulkan device");
	}
	int pipe_fds[2];
	REQUIRE(pipe(pipe_fds) == 0);
	close(pipe_fds[1]);
	struct xrt_weave_dmabuf_desc in{};
	in.fd = pipe_fds[0];
	in.width = 16;
	in.height = 16;
	in.drm_fourcc = DRM_FORMAT_ABGR8888;
	in.drm_modifier = DRM_FORMAT_MOD_LINEAR;
	in.plane_count = 1;
	in.strides[0] = 64;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	// vktest_init_bundle enables no dma-buf extensions at all.
	CHECK(vk_create_image_from_dmabuf(vk.get(), &in, VK_IMAGE_USAGE_SAMPLED_BIT, &image, &memory, nullptr) !=
	      VK_SUCCESS);
	CHECK(fd_is_closed(pipe_fds[0]));
}

#else

TEST_CASE("vk_dmabuf", "[vk_dmabuf]")
{
	SKIP("desktop Linux + Vulkan only");
}

#endif
