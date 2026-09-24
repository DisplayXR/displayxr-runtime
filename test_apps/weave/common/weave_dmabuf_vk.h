// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Client side of the XR_DXR_weave spec-v10 dma-buf transport (#1699):
 *         the Vulkan calls a desktop-Linux present-owner makes to hand the
 *         weave service a dma-buf input and to import the woven output.
 *
 * Header-only and shared by the two Linux weave clients, so they cannot drift:
 *
 *  - probes/weave_probe_vk_linux     headless contract probe (CPU-verified)
 *  - weave/weave_present_vk_linux    windowed present-owner (the browser's GPU
 *                                    process in miniature)
 *
 * The apps link the OpenXR loader + Vulkan only, so the few calls the
 * runtime's aux_vk dma-buf helpers make are written out here (same shapes as
 * src/xrt/auxiliary/vk/vk_dmabuf.c). What a producer must honour:
 *
 *  - EXPORT: an image with VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, created
 *    from the list of modifiers the device can export for the usage, and a
 *    dedicated allocation exported as ONE dma-buf fd; the descriptor carries
 *    the modifier + plane-0 layout the driver picked.
 *  - OWNERSHIP across the process boundary: release to
 *    VK_QUEUE_FAMILY_FOREIGN_EXT in GENERAL after writing; acquire back from
 *    FOREIGN before the next write. Same for the imported woven output.
 *  - SYNC: an exportable SYNC_FD semaphore signalled by the producing submit
 *    is the acquire fence; the release fence the service returns is imported
 *    TEMPORARILY into a semaphore and waited by the consuming submit.
 */
#pragma once

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN 1
#include <openxr/XR_DXR_weave.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <dirent.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif
#define WEAVE_FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
//! DRM_FORMAT_ARGB8888: bytes B,G,R,A = VK_FORMAT_B8G8R8A8_UNORM.
static const uint32_t kWeaveFourccARGB8888 = WEAVE_FOURCC('A', 'R', '2', '4');
//! DRM_FORMAT_ABGR8888: bytes R,G,B,A = VK_FORMAT_R8G8B8A8_UNORM.
static const uint32_t kWeaveFourccABGR8888 = WEAVE_FOURCC('A', 'B', '2', '4');

//! Usage every shared image carries (the probe's stage-B contract).
static const VkImageUsageFlags kWeaveDmabufUsage =
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

//! Device extensions a dma-buf weave client needs on its own VkDevice.
static const char *const kWeaveDmabufDeviceExtensions[] = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,      VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
};

/*!
 * The device a client shares buffers from, plus the extension entry points.
 * Fill phys/device/mem_props, then load().
 */
struct WeaveDmabufDevice
{
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkPhysicalDeviceMemoryProperties mem_props = {};
	PFN_vkGetMemoryFdKHR get_memory_fd = nullptr;
	PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier = nullptr;
	PFN_vkGetMemoryFdPropertiesKHR get_fd_props = nullptr;
	PFN_vkImportSemaphoreFdKHR import_sem_fd = nullptr;
	PFN_vkGetSemaphoreFdKHR get_sem_fd = nullptr;

	bool
	load(VkPhysicalDevice p, VkDevice d)
	{
		phys = p;
		device = d;
		vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
		get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR");
		get_modifier = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
		    device, "vkGetImageDrmFormatModifierPropertiesEXT");
		get_fd_props =
		    (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR");
		import_sem_fd = (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(device, "vkImportSemaphoreFdKHR");
		get_sem_fd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR");
		return get_memory_fd && get_modifier && get_fd_props && import_sem_fd && get_sem_fd;
	}
};

//! One shared image: an exported input/overlay, or the imported woven output.
struct DmabufImage
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	uint32_t w = 0, h = 0;
	VkFormat format = VK_FORMAT_UNDEFINED;
	uint32_t fourcc = 0;
	uint64_t modifier = 0;
	uint32_t offset = 0, stride = 0;
	int fd = -1;             //!< The producer's own dma-buf fd (inputs); a dup() is handed
	                         //!< over per submit.
	bool owned_by_us = true; //!< false once released to VK_QUEUE_FAMILY_FOREIGN_EXT
};

#define WEAVE_VK_CHECK(call)                                                                                           \
	do {                                                                                                           \
		VkResult _v = (call);                                                                                  \
		if (_v != VK_SUCCESS) {                                                                                \
			fprintf(stderr, "[weave_dmabuf] FAILED %s -> %d\n", #call, (int)_v);                           \
			return false;                                                                                  \
		}                                                                                                      \
	} while (0)

static inline bool
weave_find_memory_type(const WeaveDmabufDevice &d, uint32_t bits, VkMemoryPropertyFlags want, uint32_t *out)
{
	for (uint32_t i = 0; i < d.mem_props.memoryTypeCount; i++) {
		if ((bits & (1u << i)) && (d.mem_props.memoryTypes[i].propertyFlags & want) == want) {
			*out = i;
			return true;
		}
	}
	return false;
}

//! Modifiers the device can EXPORT for @p format + kWeaveDmabufUsage.
static inline std::vector<uint64_t>
weave_exportable_modifiers(const WeaveDmabufDevice &d, VkFormat format)
{
	VkDrmFormatModifierPropertiesListEXT list = {VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
	VkFormatProperties2 fp = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
	fp.pNext = &list;
	vkGetPhysicalDeviceFormatProperties2(d.phys, format, &fp);
	std::vector<VkDrmFormatModifierPropertiesEXT> props(list.drmFormatModifierCount);
	list.pDrmFormatModifierProperties = props.data();
	vkGetPhysicalDeviceFormatProperties2(d.phys, format, &fp);
	std::vector<uint64_t> out;
	for (const auto &p : props) {
		VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {
		    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
		mi.drmFormatModifier = p.drmFormatModifier;
		mi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VkPhysicalDeviceExternalImageFormatInfo ei = {
		    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
		ei.pNext = &mi;
		ei.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		VkPhysicalDeviceImageFormatInfo2 ii = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
		ii.pNext = &ei;
		ii.format = format;
		ii.type = VK_IMAGE_TYPE_2D;
		ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
		ii.usage = kWeaveDmabufUsage;
		VkExternalImageFormatProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
		VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
		ip.pNext = &ep;
		if (vkGetPhysicalDeviceImageFormatProperties2(d.phys, &ii, &ip) != VK_SUCCESS) {
			continue;
		}
		if (!(ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
			continue;
		}
		if (p.drmFormatModifierPlaneCount != 1) {
			continue; // one memory plane described
		}
		out.push_back(p.drmFormatModifier);
	}
	return out;
}

//! Exportable dma-buf image, driver-picked modifier (or LINEAR); exports ONE
//! fd.
static inline bool
weave_create_dmabuf_image(const WeaveDmabufDevice &d,
                          uint32_t w,
                          uint32_t h,
                          VkFormat format,
                          uint32_t fourcc,
                          bool force_linear,
                          DmabufImage &out)
{
	std::vector<uint64_t> mods;
	if (!force_linear) {
		mods = weave_exportable_modifiers(d, format);
	}
	if (mods.empty()) {
		mods.push_back(DRM_FORMAT_MOD_LINEAR);
	}
	VkImageDrmFormatModifierListCreateInfoEXT ml = {
	    VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
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
	ici.usage = kWeaveDmabufUsage;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	WEAVE_VK_CHECK(vkCreateImage(d.device, &ici, nullptr, &out.image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(d.device, out.image, &req);
	uint32_t type = 0;
	if (!weave_find_memory_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
		fprintf(stderr, "[weave_dmabuf] no device-local memory type for the dma-buf\n");
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
	WEAVE_VK_CHECK(vkAllocateMemory(d.device, &mai, nullptr, &out.memory));
	WEAVE_VK_CHECK(vkBindImageMemory(d.device, out.image, out.memory, 0));

	VkImageDrmFormatModifierPropertiesEXT mp = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
	WEAVE_VK_CHECK(d.get_modifier(d.device, out.image, &mp));
	VkImageSubresource sub = {VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0};
	VkSubresourceLayout layout = {};
	vkGetImageSubresourceLayout(d.device, out.image, &sub, &layout);

	VkMemoryGetFdInfoKHR gfi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	gfi.memory = out.memory;
	gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	WEAVE_VK_CHECK(d.get_memory_fd(d.device, &gfi, &out.fd));
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

/*!
 * Import the service's woven dma-buf output through its descriptor (explicit
 * modifier). Consumes desc.fd on success. The imported image is a transfer
 * source only — it is copied, never sampled or rendered to.
 */
static inline bool
weave_import_dmabuf_output(const WeaveDmabufDevice &d, const XrWeaveOutputDmabufDXR &desc, DmabufImage &out)
{
	VkFormat format = desc.drmFourcc == kWeaveFourccABGR8888 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
	std::vector<VkSubresourceLayout> planes(desc.planeCount);
	for (uint32_t i = 0; i < desc.planeCount; i++) {
		planes[i] = {};
		planes[i].offset = desc.offsets[i];
		planes[i].rowPitch = desc.strides[i];
	}
	VkImageDrmFormatModifierExplicitCreateInfoEXT ex = {
	    VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
	ex.drmFormatModifier = desc.drmModifier;
	ex.drmFormatModifierPlaneCount = desc.planeCount;
	ex.pPlaneLayouts = planes.data();
	VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	ext.pNext = &ex;
	ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.pNext = &ext;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = format;
	ici.extent = {desc.width, desc.height, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	WEAVE_VK_CHECK(vkCreateImage(d.device, &ici, nullptr, &out.image));
	VkMemoryFdPropertiesKHR fdp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
	WEAVE_VK_CHECK(d.get_fd_props(d.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, desc.fd, &fdp));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(d.device, out.image, &req);
	uint32_t type = 0;
	if (!weave_find_memory_type(d, req.memoryTypeBits & fdp.memoryTypeBits, 0, &type)) {
		fprintf(stderr, "[weave_dmabuf] no memory type can import the woven dma-buf\n");
		return false;
	}
	VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	ded.image = out.image;
	VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
	imp.pNext = &ded;
	imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	imp.fd = desc.fd;
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext = &imp;
	mai.allocationSize = req.size > desc.size ? req.size : desc.size;
	mai.memoryTypeIndex = type;
	WEAVE_VK_CHECK(vkAllocateMemory(d.device, &mai, nullptr, &out.memory));
	WEAVE_VK_CHECK(vkBindImageMemory(d.device, out.image, out.memory, 0));
	out.w = desc.width;
	out.h = desc.height;
	out.format = format;
	out.fourcc = desc.drmFourcc;
	out.modifier = desc.drmModifier;
	return true;
}

static inline void
weave_destroy_dmabuf_image(VkDevice device, DmabufImage &img)
{
	if (img.image != VK_NULL_HANDLE) {
		vkDestroyImage(device, img.image, nullptr);
	}
	if (img.memory != VK_NULL_HANDLE) {
		vkFreeMemory(device, img.memory, nullptr);
	}
	if (img.fd >= 0) {
		close(img.fd);
	}
	img = DmabufImage{};
}

//! A binary semaphore whose payload travels as a sync_file (@p exportable: we
//! signal it and export).
static inline bool
weave_create_sync_fd_semaphore(VkDevice device, bool exportable, VkSemaphore *out)
{
	VkExportSemaphoreCreateInfo esci = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
	esci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
	VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	sci.pNext = exportable ? &esci : nullptr;
	WEAVE_VK_CHECK(vkCreateSemaphore(device, &sci, nullptr, out));
	return true;
}

/*!
 * Queue-family ownership transfer with the other process: @p acquire takes
 * the image back from VK_QUEUE_FAMILY_FOREIGN_EXT, else it is released to it.
 */
static inline void
weave_foreign_barrier(VkCommandBuffer cmd,
                      uint32_t qfi,
                      VkImage image,
                      bool acquire,
                      VkImageLayout old_l,
                      VkImageLayout new_l,
                      VkAccessFlags access,
                      VkPipelineStageFlags stage)
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

//! Fill a v10 descriptor (input or overlay — same field layout) for @p img,
//! handing over @p fd.
static inline void
weave_fill_dmabuf_desc(XrWeaveDmabufDescDXR &d, const DmabufImage &img, int fd, uint64_t buffer_id)
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

static inline void
weave_fill_overlay_desc(XrWeaveOverlayDmabufDescDXR &d, const DmabufImage &img, int fd, uint64_t buffer_id)
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

//! Open fds of this process (or of @p pid), for the flat-fd-count check.
static inline int
weave_count_fds(long pid)
{
	char path[64];
	if (pid <= 0) {
		snprintf(path, sizeof(path), "/proc/self/fd");
	} else {
		snprintf(path, sizeof(path), "/proc/%ld/fd", pid);
	}
	DIR *dir = opendir(path);
	if (dir == nullptr) {
		return -1;
	}
	int n = 0;
	while (struct dirent *e = readdir(dir)) {
		if (e->d_name[0] != '.') {
			n++;
		}
	}
	closedir(dir);
	return n - (pid <= 0 ? 1 : 0); // our own opendir fd
}
