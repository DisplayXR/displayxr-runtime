// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 P5 tests: a vk_bundle with the service device's extension
 *         set (null_compositor.c), on macOS (MoltenVK) and desktop Linux, plus
 *         tiny raw-Vulkan helpers (image, upload, readback) the S5 stage test
 *         uses so it does not depend on the glue it is a building block of.
 */

#pragma once

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_vulkan_includes.h"

#include "util/comp_vulkan.h"
#include "util/u_misc.h"
#include "util/u_string_list.h"
#include "vk/vk_cmd_pool.h"
#include "vk/vk_helpers.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace vktest_conv {

inline void
bundle_destroy(struct vk_bundle *vk)
{
	if (vk == nullptr) {
		return;
	}
	if (vk->device != VK_NULL_HANDLE) {
		vk->vkDeviceWaitIdle(vk->device);
		vk->vkDestroyDevice(vk->device, nullptr);
	}
	if (vk->instance != VK_NULL_HANDLE) {
		vk->vkDestroyInstance(vk->instance, nullptr);
	}
	vk_deinit_mutex(vk);
	delete vk;
}

struct BundleDeleter
{
	void
	operator()(struct vk_bundle *vk) const
	{
		bundle_destroy(vk);
	}
};
using unique_bundle = std::unique_ptr<struct vk_bundle, BundleDeleter>;

/*!
 * A bundle on the default device with the service's external-memory / sync
 * extensions (each optional except the base set). nullptr = no Vulkan device.
 */
inline unique_bundle
make_bundle()
{
	static const char *inst_req[] = {
	    VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
	    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
	    VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
	    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
	};
	static const char *dev_req[] = {
	    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,      //
	    VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,            //
	    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,           //
	    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,        //
	    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, //
	};
	static const char *dev_opt[] = {
#ifdef VK_KHR_timeline_semaphore
		VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
#endif
#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_FD)
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
		VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
#endif
#if defined(XRT_OS_LINUX_DESKTOP)
		VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
		VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
		VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
		VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
		VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
		VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
		VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
#endif
#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_METAL)
		VK_EXT_EXTERNAL_MEMORY_METAL_EXTENSION_NAME,
		VK_EXT_METAL_OBJECTS_EXTENSION_NAME,
#endif
		VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
	};

	struct u_string_list *ir = u_string_list_create_from_array(inst_req, ARRAY_SIZE(inst_req));
	struct u_string_list *io = u_string_list_create();
#ifdef VK_KHR_portability_enumeration
	u_string_list_append(io, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif
	struct u_string_list *dr = u_string_list_create_from_array(dev_req, ARRAY_SIZE(dev_req));
	struct u_string_list *dopt = u_string_list_create_from_array(dev_opt, ARRAY_SIZE(dev_opt));

	unique_bundle vk{new vk_bundle};
	U_ZERO(vk.get());
	struct comp_vulkan_arguments args;
	memset(&args, 0, sizeof(args));
	args.required_instance_version = VK_MAKE_VERSION(1, 0, 0);
	args.get_instance_proc_address = vkGetInstanceProcAddr;
	args.required_instance_extensions = ir;
	args.optional_instance_extensions = io;
	args.required_device_extensions = dr;
	args.optional_device_extensions = dopt;
	args.log_level = U_LOGGING_WARN;
	args.only_compute_queue = false;
	args.timeline_semaphore = true;
	args.selected_gpu_index = -1;
	args.client_gpu_index = -1;
	struct comp_vulkan_results results;
	memset(&results, 0, sizeof(results));
	const bool ok = comp_vulkan_init_bundle(vk.get(), &args, &results);
	u_string_list_destroy(&ir);
	u_string_list_destroy(&io);
	u_string_list_destroy(&dr);
	u_string_list_destroy(&dopt);
	if (!ok) {
		// Nothing to own beyond what init tore down itself.
		vk->device = VK_NULL_HANDLE;
		vk->instance = VK_NULL_HANDLE;
		delete vk.release();
		return nullptr;
	}
	return vk;
}

//! One raw image + memory + 2D view (layer 0).
struct Image
{
	struct vk_bundle *vk = nullptr;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	uint32_t w = 0, h = 0, layers = 1;

	Image() = default;
	Image(const Image &) = delete;
	Image &
	operator=(const Image &) = delete;
	~Image()
	{
		if (vk == nullptr) {
			return;
		}
		if (view != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, view, nullptr);
		}
		if (image != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, image, nullptr);
		}
		if (mem != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, mem, nullptr);
		}
	}
};

inline bool
make_image(struct vk_bundle *vk, Image &img, uint32_t w, uint32_t h, uint32_t layers, VkFormat format)
{
	img.vk = vk;
	img.w = w;
	img.h = h;
	img.layers = layers;
	img.format = format;
	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = format;
	ici.extent = {w, h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = layers;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vk->vkCreateImage(vk->device, &ici, nullptr, &img.image) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements req;
	vk->vkGetImageMemoryRequirements(vk->device, img.image, &req);
	if (vk_alloc_and_bind_image_memory(vk, img.image, &req, nullptr, "vktest_conv", &img.mem) != VK_SUCCESS) {
		return false;
	}
	VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	return vk_create_view(vk, img.image, VK_IMAGE_VIEW_TYPE_2D, format, range, &img.view) == VK_SUCCESS;
}

//! A host-visible buffer for transfers.
struct Buffer
{
	struct vk_bundle *vk = nullptr;
	VkBuffer buf = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	void *map = nullptr;
	Buffer() = default;
	Buffer(const Buffer &) = delete;
	Buffer &
	operator=(const Buffer &) = delete;
	~Buffer()
	{
		if (vk == nullptr) {
			return;
		}
		if (map != nullptr) {
			vk->vkUnmapMemory(vk->device, mem);
		}
		if (buf != VK_NULL_HANDLE) {
			vk->vkDestroyBuffer(vk->device, buf, nullptr);
		}
		if (mem != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, mem, nullptr);
		}
	}
};

inline bool
make_buffer(struct vk_bundle *vk, Buffer &b, VkDeviceSize size)
{
	b.vk = vk;
	if (!vk_buffer_init(vk, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &b.buf,
	                    &b.mem)) {
		return false;
	}
	return vk->vkMapMemory(vk->device, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map) == VK_SUCCESS;
}

//! Record into a fresh command buffer, submit, wait.
template <typename F>
inline bool
run_cmd(struct vk_bundle *vk, struct vk_cmd_pool *pool, F &&record)
{
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	vk_cmd_pool_lock(pool);
	VkResult r = vk_cmd_pool_create_and_begin_cmd_buffer_locked(vk, pool, 0, &cmd);
	if (r != VK_SUCCESS) {
		vk_cmd_pool_unlock(pool);
		return false;
	}
	const bool ok = record(cmd);
	r = vk_cmd_pool_end_submit_wait_and_free_cmd_buffer_locked(vk, pool, cmd);
	vk_cmd_pool_unlock(pool);
	return ok && r == VK_SUCCESS;
}

inline void
barrier_all(struct vk_bundle *vk, VkCommandBuffer cmd)
{
	VkMemoryBarrier mb = {};
	mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb,
	                         0, nullptr, 0, nullptr);
}

//! Upload tightly packed @p data (all layers) into @p img, leaving it GENERAL.
inline bool
upload(struct vk_bundle *vk, struct vk_cmd_pool *pool, Image &img, const void *data, size_t bytes)
{
	Buffer b;
	if (!make_buffer(vk, b, bytes)) {
		return false;
	}
	memcpy(b.map, data, bytes);
	return run_cmd(vk, pool, [&](VkCommandBuffer cmd) {
		VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, img.layers};
		vk_cmd_image_barrier_locked(vk, cmd, img.image, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
		                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, range);
		VkBufferImageCopy region = {};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, img.layers};
		region.imageExtent = {img.w, img.h, 1};
		vk->vkCmdCopyBufferToImage(cmd, b.buf, img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
		barrier_all(vk, cmd);
		return true;
	});
}

//! Read every layer of @p img (in GENERAL) back, tightly packed.
inline bool
readback(struct vk_bundle *vk, struct vk_cmd_pool *pool, Image &img, void *out, size_t bytes)
{
	Buffer b;
	if (!make_buffer(vk, b, bytes)) {
		return false;
	}
	const bool ok = run_cmd(vk, pool, [&](VkCommandBuffer cmd) {
		barrier_all(vk, cmd);
		VkBufferImageCopy region = {};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, img.layers};
		region.imageExtent = {img.w, img.h, 1};
		vk->vkCmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, b.buf, 1, &region);
		barrier_all(vk, cmd);
		return true;
	});
	if (ok) {
		memcpy(out, b.map, bytes);
	}
	return ok;
}

//! float -> IEEE half -> float, round to nearest even (what an RGBA16F store does).
inline float
quantize_half(float f)
{
	uint32_t x;
	memcpy(&x, &f, 4);
	const uint32_t sign = x & 0x80000000u;
	uint32_t ax = x & 0x7fffffffu;
	uint16_t h;
	if (ax >= 0x7f800000u) {
		h = (uint16_t)((sign >> 16) | 0x7c00u | (ax > 0x7f800000u ? 0x200u : 0u));
	} else if (ax >= 0x477ff000u) {
		h = (uint16_t)((sign >> 16) | 0x7c00u); // overflow -> inf
	} else if (ax < 0x38800000u) {
		// subnormal half (or zero)
		float a;
		memcpy(&a, &ax, 4);
		const float scaled = a * 16777216.0f; // 2^24
		uint32_t m = (uint32_t)scaled;
		const float rem = scaled - (float)m;
		if (rem > 0.5f || (rem == 0.5f && (m & 1u))) {
			m++;
		}
		h = (uint16_t)((sign >> 16) | m);
	} else {
		const uint32_t e = ((ax >> 23) - 127 + 15) << 10;
		uint32_t m = ax & 0x7fffffu;
		uint32_t hm = m >> 13;
		const uint32_t rem = m & 0x1fffu;
		uint32_t v = e | hm;
		if (rem > 0x1000u || (rem == 0x1000u && (hm & 1u))) {
			v++;
		}
		h = (uint16_t)((sign >> 16) | v);
	}
	// back to float
	const uint32_t hs = (uint32_t)(h & 0x8000u) << 16;
	uint32_t he = (h >> 10) & 0x1fu;
	uint32_t hmn = h & 0x3ffu;
	uint32_t bits;
	if (he == 0) {
		if (hmn == 0) {
			bits = hs;
		} else {
			he = 127 - 15 + 1;
			while ((hmn & 0x400u) == 0) {
				hmn <<= 1;
				he--;
			}
			hmn &= 0x3ffu;
			bits = hs | (he << 23) | (hmn << 13);
		}
	} else if (he == 0x1f) {
		bits = hs | 0x7f800000u | (hmn << 13);
	} else {
		bits = hs | ((he + 127 - 15) << 23) | (hmn << 13);
	}
	float r;
	memcpy(&r, &bits, 4);
	return r;
}

inline uint16_t
to_half_bits(float f)
{
	const float q = quantize_half(f);
	// Re-encode the (now exactly representable) value.
	uint32_t x;
	memcpy(&x, &q, 4);
	const uint32_t sign = (x >> 16) & 0x8000u;
	const uint32_t ax = x & 0x7fffffffu;
	if (ax == 0) {
		return (uint16_t)sign;
	}
	if (ax >= 0x7f800000u) {
		return (uint16_t)(sign | 0x7c00u | (ax > 0x7f800000u ? 0x200u : 0u));
	}
	if (ax < 0x38800000u) {
		float a;
		memcpy(&a, &ax, 4);
		return (uint16_t)(sign | (uint32_t)(a * 16777216.0f));
	}
	return (uint16_t)(sign | ((((ax >> 23) - 127 + 15) << 10) | ((ax & 0x7fffffu) >> 13)));
}

inline float
half_bits_to_float(uint16_t h)
{
	const uint32_t hs = (uint32_t)(h & 0x8000u) << 16;
	uint32_t he = (h >> 10) & 0x1fu;
	uint32_t hm = h & 0x3ffu;
	uint32_t bits;
	if (he == 0) {
		if (hm == 0) {
			bits = hs;
		} else {
			he = 127 - 15 + 1;
			while ((hm & 0x400u) == 0) {
				hm <<= 1;
				he--;
			}
			hm &= 0x3ffu;
			bits = hs | (he << 23) | (hm << 13);
		}
	} else if (he == 0x1f) {
		bits = hs | 0x7f800000u | (hm << 13);
	} else {
		bits = hs | ((he + 127 - 15) << 23) | (hm << 13);
	}
	float r;
	memcpy(&r, &bits, 4);
	return r;
}

} // namespace vktest_conv
