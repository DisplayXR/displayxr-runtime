// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Consumer half of the cross-process dma-buf zero-copy spike (epic #1699).
 *
 * Headless Vulkan on the GPU behind the given render node. Receives plane fds +
 * DRM format modifier + acquire sync_file over SCM_RIGHTS, imports the dma-buf as
 * a VkImage (VK_EXT_image_drm_format_modifier explicit plane layouts, dedicated
 * VkImportMemoryFdInfoKHR allocation), acquires it from VK_QUEUE_FAMILY_FOREIGN_EXT,
 * SAMPLES it in a compute shader through a VkImageView + VkSampler into a
 * host-visible buffer, returns a release sync_file, and verifies every pixel.
 */
#include "common.h"
#include "vkcommon.h"

#include "sample_comp_spv.h"

#include <sys/stat.h>

struct copts
{
	const char *render_node, *sock_path;
	int want_cpu, cache, list_only, sparse;
	int force_mod_set;
	uint64_t force_mod; /* negative control: import with a WRONG modifier */
	const char *rp;      /* return path: "linear" or "list:hex,hex,..." (Vulkan allocates + exports) */
	uint32_t rp_fourcc, rp_w, rp_h;
	int rp_frames, rp_fps;
};
static struct copts g_o;

static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t qf;
static PFN_vkGetMemoryFdPropertiesKHR p_vkGetMemoryFdPropertiesKHR;
static PFN_vkImportSemaphoreFdKHR p_vkImportSemaphoreFdKHR;
static PFN_vkGetSemaphoreFdKHR p_vkGetSemaphoreFdKHR;
static PFN_vkGetMemoryFdKHR p_vkGetMemoryFdKHR;
static PFN_vkGetImageDrmFormatModifierPropertiesEXT p_vkGetImageDrmFormatModifierPropertiesEXT;

static const char *dev_exts[] = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,     VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,  VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
};

static void
print_modifier_table(void)
{
	const struct
	{
		uint32_t fourcc;
	} fmts[] = {{DRM_FORMAT_ARGB8888}, {DRM_FORMAT_XRGB8888}, {DRM_FORMAT_ABGR8888}};
	for (unsigned f = 0; f < 3; f++) {
		int ha;
		VkFormat vf = spike_fourcc_to_vk(fmts[f].fourcc, &ha);
		struct spike_mod_info *m;
		uint32_t n = spike_vk_modifiers(pd, vf, &m);
		printf("consumer: Vulkan modifiers for DRM %s (VkFormat %d), %u entries:\n", spike_fourcc_name(fmts[f].fourcc),
		       vf, n);
		for (uint32_t i = 0; i < n; i++) {
			VkExternalMemoryFeatureFlags ef = 0;
			VkResult r = spike_vk_mod_image_ok(pd, vf, m[i].modifier, VK_IMAGE_USAGE_SAMPLED_BIT, &ef);
			printf("  %-58s planes=%u SAMPLED=%s COLOR_ATTACHMENT=%s  dma-buf-import+SAMPLED: %s%s\n",
			       spike_mod_name(m[i].modifier), m[i].planes,
			       (m[i].features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) ? "yes" : "no",
			       (m[i].features & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT) ? "yes" : "no",
			       r == VK_SUCCESS ? "ok" : spike_vk_result(r),
			       (r == VK_SUCCESS && !(ef & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) ? " (NOT importable)" : "");
		}
		free(m);
	}
}

static uint32_t
find_mem(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (int pass = 0; pass < 2; pass++)
		for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
			VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
			if ((bits & (1u << i)) && (f & want) == want && (pass || (f & nice) == nice))
				return i;
		}
	return UINT32_MAX;
}

struct imported
{
	uint32_t bo_id;
	VkImage image;
	VkDeviceMemory mem;
	VkImageView view;
};

static void
destroy_imported(struct imported *im)
{
	if (im->view) vkDestroyImageView(dev, im->view, NULL);
	if (im->image) vkDestroyImage(dev, im->image, NULL);
	if (im->mem) vkFreeMemory(dev, im->mem, NULL);
	memset(im, 0, sizeof(*im));
}

static int g_same_inode_checked, g_planes_share_inode = 1;

/*
 * Import. Consumes every fd in plane_fds (either transferred to Vulkan or closed).
 * Returns VK_SUCCESS or the failing VkResult, with *where set.
 */
static VkResult
import_dmabuf(const struct spike_frame_msg *m, int *plane_fds, struct imported *out, const char **where)
{
	memset(out, 0, sizeof(*out));
	out->bo_id = m->bo_id;
	int ha;
	VkFormat vf = spike_fourcc_to_vk(m->fourcc, &ha);
	VkResult r;

	/* All planes of a single (non-disjoint) bo must be the same dma-buf. */
	struct stat s0;
	fstat(plane_fds[0], &s0);
	for (uint32_t i = 1; i < m->num_planes; i++) {
		struct stat si;
		fstat(plane_fds[i], &si);
		if (si.st_ino != s0.st_ino)
			g_planes_share_inode = 0;
	}
	g_same_inode_checked = 1;

	VkSubresourceLayout pl[SPIKE_MAX_PLANES] = {0};
	for (uint32_t i = 0; i < m->num_planes; i++) {
		pl[i].offset = m->offset[i];
		pl[i].rowPitch = m->stride[i];
	}
	VkImageDrmFormatModifierExplicitCreateInfoEXT mx = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
	    .drmFormatModifier = g_o.force_mod_set ? g_o.force_mod : m->modifier,
	    .drmFormatModifierPlaneCount = m->num_planes,
	    .pPlaneLayouts = pl};
	VkExternalMemoryImageCreateInfo emi = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	                                       .pNext = &mx,
	                                       .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
	VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	                         .pNext = &emi,
	                         .imageType = VK_IMAGE_TYPE_2D,
	                         .format = vf,
	                         .extent = {m->width, m->height, 1},
	                         .mipLevels = 1,
	                         .arrayLayers = 1,
	                         .samples = VK_SAMPLE_COUNT_1_BIT,
	                         .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
	                         .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
	                         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	                         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	r = vkCreateImage(dev, &ici, NULL, &out->image);
	if (r != VK_SUCCESS) { *where = "vkCreateImage(DRM_FORMAT_MODIFIER explicit)"; goto fail; }

	VkMemoryFdPropertiesKHR fdp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
	r = p_vkGetMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, plane_fds[0], &fdp);
	if (r != VK_SUCCESS) { *where = "vkGetMemoryFdPropertiesKHR"; goto fail; }

	VkMemoryDedicatedRequirements dr = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
	VkMemoryRequirements2 mr = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dr};
	VkImageMemoryRequirementsInfo2 mri = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
	                                      .image = out->image};
	vkGetImageMemoryRequirements2(dev, &mri, &mr);
	uint32_t mt = find_mem(mr.memoryRequirements.memoryTypeBits & fdp.memoryTypeBits, 0, 0);
	if (mt == UINT32_MAX) { r = VK_ERROR_FEATURE_NOT_PRESENT; *where = "no memory type in image&fd bits"; goto fail; }

	VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
	                                     .image = out->image};
	VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
	                               .pNext = &ded,
	                               .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	                               .fd = plane_fds[0]};
	VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                            .pNext = &imp,
	                            .allocationSize = mr.memoryRequirements.size,
	                            .memoryTypeIndex = mt};
	r = vkAllocateMemory(dev, &mai, NULL, &out->mem);
	if (r != VK_SUCCESS) { *where = "vkAllocateMemory(VkImportMemoryFdInfoKHR DMA_BUF, dedicated)"; goto fail; }
	/* Vulkan spec (VkImportMemoryFdInfoKHR): a successful import transfers ownership
	 * of the fd to the implementation, for every fd handle type incl. DMA_BUF.
	 * So plane 0's fd is no longer ours; the extra plane fds (same dma-buf) are. */
	plane_fds[0] = -1;

	r = vkBindImageMemory(dev, out->image, out->mem, 0);
	if (r != VK_SUCCESS) { *where = "vkBindImageMemory"; goto fail; }

	VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	                             .image = out->image,
	                             .viewType = VK_IMAGE_VIEW_TYPE_2D,
	                             .format = vf,
	                             .components = {.a = ha ? VK_COMPONENT_SWIZZLE_IDENTITY : VK_COMPONENT_SWIZZLE_ONE},
	                             .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	r = vkCreateImageView(dev, &vci, NULL, &out->view);
	if (r != VK_SUCCESS) { *where = "vkCreateImageView"; goto fail; }
	for (uint32_t i = 1; i < m->num_planes; i++)
		close(plane_fds[i]);
	return VK_SUCCESS;
fail:
	for (uint32_t i = 0; i < m->num_planes; i++)
		if (plane_fds[i] >= 0)
			close(plane_fds[i]);
	destroy_imported(out);
	return r;
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

/*
 * Woven-output direction: Vulkan (the runtime's role) allocates an exportable
 * dma-buf image -- LINEAR, or the driver's pick from a modifier list via
 * VkImageDrmFormatModifierListCreateInfoEXT -- writes frame f's pattern into it,
 * releases it to VK_QUEUE_FAMILY_FOREIGN_EXT and ships fresh fds + an acquire
 * sync_file to the GL side (producer --return-path), which samples + verifies.
 */
static void
fill_pattern(uint8_t *dst, uint32_t w, uint32_t h, int f, int bgr)
{
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			uint8_t e[4], *d = dst + ((size_t)y * w + x) * 4;
			spike_pattern((int)x, (int)y, f, (int)w, (int)h, e);
			d[0] = bgr ? e[2] : e[0], d[1] = e[1], d[2] = bgr ? e[0] : e[2], d[3] = e[3];
		}
}

static int
run_return_path(const struct copts *o, int sock, const VkPhysicalDeviceDriverProperties *drv)
{
	int ha;
	VkFormat vf = spike_fourcc_to_vk(o->rp_fourcc, &ha);
	const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	uint64_t mods[64];
	uint32_t nm = 0;
	if (!strcmp(o->rp, "linear")) {
		mods[nm++] = DRM_FORMAT_MOD_LINEAR;
	} else if (!strncmp(o->rp, "list:", 5)) {
		char buf[1024];
		snprintf(buf, sizeof(buf), "%s", o->rp + 5);
		for (char *t = strtok(buf, ","); t && nm < 64; t = strtok(NULL, ",")) {
			uint64_t m = strtoull(t, NULL, 16);
			VkExternalMemoryFeatureFlags ef = 0;
			VkResult r = m == DRM_FORMAT_MOD_INVALID ? VK_ERROR_FORMAT_NOT_SUPPORTED
			                                         : spike_vk_mod_image_ok(pd, vf, m, usage, &ef);
			int keep = r == VK_SUCCESS && (ef & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT);
			printf("consumer[return]: candidate %s -> %s\n", spike_mod_name(m),
			       m == DRM_FORMAT_MOD_INVALID ? "dropped (INVALID is not a Vulkan modifier)"
			       : keep                     ? "kept"
			                                  : spike_vk_result(r));
			if (keep)
				mods[nm++] = m;
		}
	} else {
		fprintf(stderr, "bad --return-path=%s\n", o->rp);
		return 2;
	}
	if (!nm) {
		fprintf(stderr, "consumer[return]: empty modifier list\n");
		return 1;
	}
	VkImageDrmFormatModifierListCreateInfoEXT ml = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
	    .drmFormatModifierCount = nm,
	    .pDrmFormatModifiers = mods};
	VkExternalMemoryImageCreateInfo emi = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	                                       .pNext = &ml,
	                                       .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
	VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	                         .pNext = &emi,
	                         .imageType = VK_IMAGE_TYPE_2D,
	                         .format = vf,
	                         .extent = {o->rp_w, o->rp_h, 1},
	                         .mipLevels = 1,
	                         .arrayLayers = 1,
	                         .samples = VK_SAMPLE_COUNT_1_BIT,
	                         .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
	                         .usage = usage,
	                         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	                         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	VkImage img;
	VKCHK(vkCreateImage(dev, &ici, NULL, &img));
	VkMemoryDedicatedRequirements dr = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
	VkMemoryRequirements2 mr = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dr};
	VkImageMemoryRequirementsInfo2 mri = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = img};
	vkGetImageMemoryRequirements2(dev, &mri, &mr);
	VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = img};
	VkExportMemoryAllocateInfo exp = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
	                                  .pNext = &ded,
	                                  .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
	VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                            .pNext = &exp,
	                            .allocationSize = mr.memoryRequirements.size,
	                            .memoryTypeIndex = find_mem(mr.memoryRequirements.memoryTypeBits, 0,
	                                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
	VkDeviceMemory mem;
	VKCHK(vkAllocateMemory(dev, &mai, NULL, &mem));
	VKCHK(vkBindImageMemory(dev, img, mem, 0));
	VkImageDrmFormatModifierPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
	VKCHK(p_vkGetImageDrmFormatModifierPropertiesEXT(dev, img, &mp));
	struct spike_mod_info *all;
	uint32_t na = spike_vk_modifiers(pd, vf, &all), planes = 1;
	for (uint32_t i = 0; i < na; i++)
		if (all[i].modifier == mp.drmFormatModifier)
			planes = all[i].planes;
	free(all);
	struct spike_frame_msg tmpl = {.magic = SPIKE_MAGIC,
	                               .type = SPIKE_MSG_FRAME,
	                               .bo_id = 1,
	                               .width = o->rp_w,
	                               .height = o->rp_h,
	                               .fourcc = o->rp_fourcc,
	                               .num_planes = planes,
	                               .modifier = mp.drmFormatModifier};
	static const VkImageAspectFlags plane_aspect[4] = {
	    VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
	    VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT};
	printf("consumer[return]: exported image %ux%u %s: driver chose modifier=%s planes=%u size=%llu\n", o->rp_w,
	       o->rp_h, spike_fourcc_name(o->rp_fourcc), spike_mod_name(mp.drmFormatModifier), planes,
	       (unsigned long long)mr.memoryRequirements.size);
	for (uint32_t i = 0; i < planes; i++) {
		VkImageSubresource sr = {.aspectMask = plane_aspect[i]};
		VkSubresourceLayout l;
		vkGetImageSubresourceLayout(dev, img, &sr, &l);
		tmpl.offset[i] = (uint32_t)l.offset, tmpl.stride[i] = (uint32_t)l.rowPitch;
		printf("  plane %u: stride=%u offset=%u size=%llu\n", i, tmpl.stride[i], tmpl.offset[i],
		       (unsigned long long)l.size);
	}

	/* staging */
	VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	                          .size = (VkDeviceSize)o->rp_w * o->rp_h * 4 * 2, /* double-buffered */
	                          .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
	VkBuffer sbuf;
	VKCHK(vkCreateBuffer(dev, &bci, NULL, &sbuf));
	VkMemoryRequirements sreq;
	vkGetBufferMemoryRequirements(dev, sbuf, &sreq);
	VkMemoryAllocateInfo smai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                             .allocationSize = sreq.size,
	                             .memoryTypeIndex = find_mem(sreq.memoryTypeBits,
	                                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                                                         0)};
	VkDeviceMemory smem;
	VKCHK(vkAllocateMemory(dev, &smai, NULL, &smem));
	VKCHK(vkBindBufferMemory(dev, sbuf, smem, 0));
	uint8_t *smap;
	VKCHK(vkMapMemory(dev, smem, 0, VK_WHOLE_SIZE, 0, (void **)&smap));
	int bgr = vf == VK_FORMAT_B8G8R8A8_UNORM;

	VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	                               .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	                               .queueFamilyIndex = qf};
	VkCommandPool cp;
	VKCHK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
	VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	                                    .commandPool = cp,
	                                    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	                                    .commandBufferCount = 1};
	VkCommandBuffer cb;
	VKCHK(vkAllocateCommandBuffers(dev, &cbai, &cb));
	VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence;
	VKCHK(vkCreateFence(dev, &fci, NULL, &fence));
	VkSemaphoreCreateInfo semci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VkSemaphore rel_in;
	VKCHK(vkCreateSemaphore(dev, &semci, NULL, &rel_in));
	VkExportSemaphoreCreateInfo esci = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
	                                    .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
	VkSemaphoreCreateInfo semci_x = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &esci};
	VkSemaphore acq_out;
	VKCHK(vkCreateSemaphore(dev, &semci_x, NULL, &acq_out));

	int fd_ready = spike_count_fds(), fd_peak = fd_ready, have_rel = 0, rel_fences = 0, acq_fences = 0, failed = 0;
	uint64_t *t_rt = calloc((size_t)o->rp_frames, 8);
	uint64_t period = o->rp_fps > 0 ? 1000000000ull / (uint64_t)o->rp_fps : 0;
	const size_t half = (size_t)o->rp_w * o->rp_h * 4;
	fill_pattern(smap, o->rp_w, o->rp_h, 0, bgr);
	uint64_t t0 = spike_now_ns(), next = t0;
	for (int f = 0; f < o->rp_frames; f++) {
		uint64_t ts = spike_now_ns();
		VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		                                 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
		VKCHK(vkBeginCommandBuffer(cb, &cbbi));
		VkImageMemoryBarrier acq = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		                            .srcAccessMask = 0,
		                            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		                            .oldLayout = f == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
		                            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                            .srcQueueFamilyIndex = f == 0 ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_FOREIGN_EXT,
		                            .dstQueueFamilyIndex = f == 0 ? VK_QUEUE_FAMILY_IGNORED : qf,
		                            .image = img,
		                            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
		                     NULL, 1, &acq);
		VkBufferImageCopy reg = {.bufferOffset = (f & 1) * half,
		                         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		                         .imageExtent = {o->rp_w, o->rp_h, 1}};
		vkCmdCopyBufferToImage(cb, sbuf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
		VkImageMemoryBarrier rel = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		                            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		                            .dstAccessMask = 0,
		                            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
		                            .srcQueueFamilyIndex = qf,
		                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
		                            .image = img,
		                            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
		                     NULL, 1, &rel);
		VKCHK(vkEndCommandBuffer(cb));
		VkPipelineStageFlags wst = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		                   .waitSemaphoreCount = have_rel ? 1 : 0,
		                   .pWaitSemaphores = &rel_in,
		                   .pWaitDstStageMask = &wst,
		                   .commandBufferCount = 1,
		                   .pCommandBuffers = &cb,
		                   .signalSemaphoreCount = 1,
		                   .pSignalSemaphores = &acq_out};
		VKCHK(vkQueueSubmit(queue, 1, &si, fence));
		have_rel = 0;

		int fds[5], nfd = 0, acq_fd = -1, mem_fd = -1;
		VkSemaphoreGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
		                              .semaphore = acq_out,
		                              .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
		VKCHK(p_vkGetSemaphoreFdKHR(dev, &gi, &acq_fd));
		acq_fences++;
		VkMemoryGetFdInfoKHR mgi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
		                            .memory = mem,
		                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
		VKCHK(p_vkGetMemoryFdKHR(dev, &mgi, &mem_fd)); /* fresh fd every frame, like the browser path */
		fds[nfd++] = mem_fd;
		for (uint32_t i = 1; i < planes; i++)
			fds[nfd++] = dup(mem_fd);
		fds[nfd++] = acq_fd;
		struct spike_frame_msg m = tmpl;
		m.frame = (uint32_t)f;
		m.has_fence = 1;
		int pk = spike_count_fds();
		if (pk > fd_peak) fd_peak = pk;
		if (spike_send(sock, &m, sizeof(m), fds, nfd) != 0) {
			perror("consumer[return]: sendmsg");
			return 1;
		}
		for (int i = 0; i < nfd; i++) close(fds[i]);
		/* CPU-side test-pattern generation for the next frame, overlapped with the GL side. */
		if (f + 1 < o->rp_frames)
			fill_pattern(smap + ((f + 1) & 1) * half, o->rp_w, o->rp_h, f + 1, bgr);

		struct spike_reply_msg r;
		int rfd[2], nr;
		if (spike_recv(sock, &r, sizeof(r), rfd, 2, &nr) <= 0) {
			fprintf(stderr, "consumer[return]: GL side went away at frame %d\n", f);
			return 1;
		}
		if (r.status != 0) failed++;
		if (nr > 0) {
			VkImportSemaphoreFdInfoKHR isi = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
			                                  .semaphore = rel_in,
			                                  .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
			                                  .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
			                                  .fd = rfd[0]};
			if (p_vkImportSemaphoreFdKHR(dev, &isi) == VK_SUCCESS)
				have_rel = 1, rel_fences++;
			else
				close(rfd[0]);
			for (int i = 1; i < nr; i++) close(rfd[i]);
		}
		VKCHK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
		VKCHK(vkResetFences(dev, 1, &fence));
		t_rt[f] = spike_now_ns() - ts;
		if (period) {
			next += period;
			uint64_t now = spike_now_ns();
			if (next > now) {
				struct timespec d = {.tv_sec = (time_t)(next / 1000000000ull), .tv_nsec = (long)(next % 1000000000ull)};
				clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &d, NULL);
			} else
				next = now;
		}
	}
	double secs = (double)(spike_now_ns() - t0) / 1e9;
	struct spike_frame_msg q = {.magic = SPIKE_MAGIC, .type = SPIKE_MSG_QUIT};
	spike_send(sock, &q, sizeof(q), NULL, 0);
	vkDeviceWaitIdle(dev);
	int fd_end = spike_count_fds();
	spike_dump_fds("consumer[return]");
	qsort(t_rt, (size_t)o->rp_frames, 8, cmp_u64);
	uint64_t sum = 0;
	for (int i = 0; i < o->rp_frames; i++) sum += t_rt[i];
	printf("consumer[return]: RESULT driver='%s' '%s' format=%s modifier=%s planes=%u frames=%d fps=%.2f "
	       "gl_import_failed=%d acquire_fences=%d release_fences=%d submit+export+send+GL-reply+copy-done_us avg=%.0f p99=%.0f fds "
	       "ready=%d peak=%d end=%d\n",
	       drv->driverName, drv->driverInfo, spike_fourcc_name(o->rp_fourcc), spike_mod_name(mp.drmFormatModifier),
	       planes, o->rp_frames, o->rp_frames / secs, failed, acq_fences, rel_fences, sum / 1e3 / o->rp_frames,
	       t_rt[(o->rp_frames * 99) / 100] / 1e3, fd_ready, fd_peak, fd_end);
	vkDestroySemaphore(dev, acq_out, NULL);
	vkDestroySemaphore(dev, rel_in, NULL);
	vkDestroyFence(dev, fence, NULL);
	vkDestroyCommandPool(dev, cp, NULL);
	vkUnmapMemory(dev, smem);
	vkDestroyBuffer(dev, sbuf, NULL);
	vkFreeMemory(dev, smem, NULL);
	vkDestroyImage(dev, img, NULL);
	vkFreeMemory(dev, mem, NULL);
	close(sock);
	return failed ? 3 : 0;
}

#define MAX_FRAMES 100000

int
main(int argc, char **argv)
{
	struct copts o = {.render_node = "/dev/dri/renderD128",
	                  .sock_path = spike_default_socket(),
	                  .rp_fourcc = DRM_FORMAT_ABGR8888,
	                  .rp_w = 1920,
	                  .rp_h = 1080,
	                  .rp_frames = 600,
	                  .rp_fps = 60};
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--cpu")) o.want_cpu = 1;
		else if (!strcmp(a, "--cache-imports")) o.cache = 1;
		else if (!strcmp(a, "--list")) o.list_only = 1;
		else if (!strcmp(a, "--sparse-verify")) o.sparse = 1;
		else if (!strncmp(a, "--return-path=", 14)) o.rp = a + 14;
		else if (!strncmp(a, "--format=", 9)) o.rp_fourcc = spike_parse_fourcc(a + 9);
		else if (!strncmp(a, "--size=", 7)) sscanf(a + 7, "%ux%u", &o.rp_w, &o.rp_h);
		else if (!strncmp(a, "--frames=", 9)) o.rp_frames = atoi(a + 9);
		else if (!strncmp(a, "--fps=", 6)) o.rp_fps = atoi(a + 6);
		else if (!strncmp(a, "--force-modifier=", 17)) o.force_mod_set = 1, o.force_mod = strtoull(a + 17, NULL, 16);
		else if (!strncmp(a, "--socket=", 9)) o.sock_path = a + 9;
		else if (!strncmp(a, "--render-node=", 14)) o.render_node = a + 14;
		else {
			fprintf(stderr, "usage: %s [--list] [--cpu] [--cache-imports] [--sparse-verify] [--force-modifier=HEX] [--socket=P]\n"
			                "  return path: --return-path=linear|list:HEX,HEX.. [--format=F] [--size=WxH] [--frames=N] [--fps=N]\n", argv[0]);
			return 2;
		}
	}
	g_o = o;
	if (o.force_mod_set)
		printf("consumer: NEGATIVE CONTROL: importing with forced modifier %s\n", spike_mod_name(o.force_mod));
	int fd_start = spike_count_fds();

	VkInstance inst = spike_vk_instance();
	pd = spike_vk_pick(inst, o.render_node, o.want_cpu);
	VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
	VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
	vkGetPhysicalDeviceProperties2(pd, &p2);
	printf("consumer: device='%s' (%04x:%04x) api=%u.%u.%u driverName='%s' driverInfo='%s' driverID=%d\n",
	       p2.properties.deviceName, p2.properties.vendorID, p2.properties.deviceID,
	       VK_API_VERSION_MAJOR(p2.properties.apiVersion), VK_API_VERSION_MINOR(p2.properties.apiVersion),
	       VK_API_VERSION_PATCH(p2.properties.apiVersion), drv.driverName, drv.driverInfo, drv.driverID);
	print_modifier_table();
	if (o.list_only)
		return 0;

	for (unsigned i = 0; i < sizeof(dev_exts) / sizeof(dev_exts[0]); i++)
		if (!spike_has_dev_ext(pd, dev_exts[i])) {
			fprintf(stderr, "consumer: device lacks %s\n", dev_exts[i]);
			return 1;
		}
	uint32_t nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
	VkQueueFamilyProperties qp[16];
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
	for (qf = 0; qf < nq; qf++)
		if (qp[qf].queueFlags & VK_QUEUE_COMPUTE_BIT)
			break;
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	                               .queueFamilyIndex = qf,
	                               .queueCount = 1,
	                               .pQueuePriorities = &prio};
	VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
	                          .queueCreateInfoCount = 1,
	                          .pQueueCreateInfos = &qci,
	                          .enabledExtensionCount = sizeof(dev_exts) / sizeof(dev_exts[0]),
	                          .ppEnabledExtensionNames = dev_exts};
	VKCHK(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, qf, 0, &queue);
	p_vkGetMemoryFdPropertiesKHR = (void *)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
	p_vkImportSemaphoreFdKHR = (void *)vkGetDeviceProcAddr(dev, "vkImportSemaphoreFdKHR");
	p_vkGetSemaphoreFdKHR = (void *)vkGetDeviceProcAddr(dev, "vkGetSemaphoreFdKHR");
	p_vkGetMemoryFdKHR = (void *)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
	p_vkGetImageDrmFormatModifierPropertiesEXT =
	    (void *)vkGetDeviceProcAddr(dev, "vkGetImageDrmFormatModifierPropertiesEXT");

	/* Can we export SYNC_FD from a binary semaphore? */
	VkPhysicalDeviceExternalSemaphoreInfo esi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
	                                             .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
	VkExternalSemaphoreProperties esp = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
	vkGetPhysicalDeviceExternalSemaphoreProperties(pd, &esi, &esp);
	int can_export = !!(esp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT);
	int can_import = !!(esp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT);
	printf("consumer: SYNC_FD semaphore import=%d export=%d\n", can_import, can_export);

	/* Pipeline. */
	VkDescriptorSetLayoutBinding b[2] = {
	    {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	    {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
	VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	                                      .bindingCount = 2,
	                                      .pBindings = b};
	VkDescriptorSetLayout dsl;
	VKCHK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &dsl));
	VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
	VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	                                   .setLayoutCount = 1,
	                                   .pSetLayouts = &dsl,
	                                   .pushConstantRangeCount = 1,
	                                   .pPushConstantRanges = &pcr};
	VkPipelineLayout pl;
	VKCHK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
	VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	                                 .codeSize = sizeof(sample_comp_spv),
	                                 .pCode = sample_comp_spv};
	VkShaderModule sm;
	VKCHK(vkCreateShaderModule(dev, &smci, NULL, &sm));
	VkComputePipelineCreateInfo cpci = {
	    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
	    .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	              .stage = VK_SHADER_STAGE_COMPUTE_BIT,
	              .module = sm,
	              .pName = "main"},
	    .layout = pl};
	VkPipeline pipe;
	VKCHK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
	VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	                           .magFilter = VK_FILTER_NEAREST,
	                           .minFilter = VK_FILTER_NEAREST,
	                           .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
	                           .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                           .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                           .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
	VkSampler sampler;
	VKCHK(vkCreateSampler(dev, &sci, NULL, &sampler));
	VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
	VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	                                   .maxSets = 1,
	                                   .poolSizeCount = 2,
	                                   .pPoolSizes = ps};
	VkDescriptorPool dp;
	VKCHK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
	VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	                                    .descriptorPool = dp,
	                                    .descriptorSetCount = 1,
	                                    .pSetLayouts = &dsl};
	VkDescriptorSet ds;
	VKCHK(vkAllocateDescriptorSets(dev, &dsai, &ds));
	VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	                               .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	                               .queueFamilyIndex = qf};
	VkCommandPool cp;
	VKCHK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
	VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	                                    .commandPool = cp,
	                                    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	                                    .commandBufferCount = 1};
	VkCommandBuffer cb;
	VKCHK(vkAllocateCommandBuffers(dev, &cbai, &cb));
	VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence;
	VKCHK(vkCreateFence(dev, &fci, NULL, &fence));
	VkQueryPoolCreateInfo qpci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
	                              .queryType = VK_QUERY_TYPE_TIMESTAMP,
	                              .queryCount = 2};
	VkQueryPool qpool;
	VKCHK(vkCreateQueryPool(dev, &qpci, NULL, &qpool));
	double ts_period_ns = p2.properties.limits.timestampPeriod;
	int have_ts = qp[qf].timestampValidBits != 0;
	VkSemaphoreCreateInfo semci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VkSemaphore acq_sem;
	VKCHK(vkCreateSemaphore(dev, &semci, NULL, &acq_sem));
	VkExportSemaphoreCreateInfo esci = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
	                                    .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
	VkSemaphoreCreateInfo semci_x = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &esci};
	VkSemaphore rel_sem = VK_NULL_HANDLE;
	if (can_export)
		VKCHK(vkCreateSemaphore(dev, &semci_x, NULL, &rel_sem));

	int sfd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	struct sockaddr_un sa = {.sun_family = AF_UNIX};
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%.100s", o.sock_path);
	unlink(o.sock_path);
	if (bind(sfd, (struct sockaddr *)&sa, sizeof(sa)) || listen(sfd, 1)) { perror("bind/listen"); return 1; }
	printf("consumer: listening on %s\n", o.sock_path);
	fflush(stdout);
	int sock = accept4(sfd, NULL, NULL, SOCK_CLOEXEC);
	close(sfd);
	unlink(o.sock_path);

	if (o.rp)
		return run_return_path(&o, sock, &drv);

	int fd_ready = spike_count_fds(), fd_peak = fd_ready;
	VkBuffer obuf = VK_NULL_HANDLE;
	VkDeviceMemory omem = VK_NULL_HANDLE;
	uint32_t *omap = NULL, ow = 0, oh = 0;
	struct imported cache = {0};

	uint64_t *t_import = calloc(MAX_FRAMES, 8), *t_total = calloc(MAX_FRAMES, 8), *t_verify = calloc(MAX_FRAMES, 8), *t_gpu = calloc(MAX_FRAMES, 8);
	int frames = 0, bad_frames = 0, fail_frames = 0, acq_fenced = 0, rel_fenced = 0, cache_hits = 0, printed = 0;
	uint64_t bad_pixels = 0, first_mod = 0, n_mod_seen = 0;
	uint32_t first_planes = 0, first_fourcc = 0;
	const char *last_fail = NULL;
	VkResult last_fail_r = VK_SUCCESS;

	for (;;) {
		struct spike_frame_msg m;
		int fds[8], nfd;
		ssize_t n = spike_recv(sock, &m, sizeof(m), fds, 8, &nfd);
		uint64_t t0 = spike_now_ns();
		if (n <= 0 || m.magic != SPIKE_MAGIC || m.type == SPIKE_MSG_QUIT) {
			for (int i = 0; i < nfd; i++) close(fds[i]);
			break;
		}
		int pk = spike_count_fds();
		if (pk > fd_peak) fd_peak = pk;
		if (nfd != (int)m.num_planes + (m.has_fence ? 1 : 0)) {
			fprintf(stderr, "consumer: frame %u: got %d fds, expected %u+%d\n", m.frame, nfd, m.num_planes,
			        m.has_fence);
			return 1;
		}
		int acq_fd = m.has_fence ? fds[m.num_planes] : -1;
		if (!n_mod_seen++) {
			first_mod = m.modifier, first_planes = m.num_planes, first_fourcc = m.fourcc;
			printf("consumer: first frame: %ux%u %s modifier=%s planes=%u acquire_fence=%d\n", m.width, m.height,
			       spike_fourcc_name(m.fourcc), spike_mod_name(m.modifier), m.num_planes, m.has_fence);
			for (uint32_t i = 0; i < m.num_planes; i++)
				printf("  plane %u: stride=%u offset=%u\n", i, m.stride[i], m.offset[i]);
		}

		if (!omap || ow != m.width || oh != m.height) {
			if (omap) {
				vkUnmapMemory(dev, omem), vkDestroyBuffer(dev, obuf, NULL), vkFreeMemory(dev, omem, NULL);
			}
			ow = m.width, oh = m.height;
			VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			                          .size = (VkDeviceSize)ow * oh * 4,
			                          .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
			VKCHK(vkCreateBuffer(dev, &bci, NULL, &obuf));
			VkMemoryRequirements req;
			vkGetBufferMemoryRequirements(dev, obuf, &req);
			VkMemoryAllocateInfo mai = {
			    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			    .allocationSize = req.size,
			    .memoryTypeIndex = find_mem(req.memoryTypeBits,
			                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			                                VK_MEMORY_PROPERTY_HOST_CACHED_BIT)};
			VKCHK(vkAllocateMemory(dev, &mai, NULL, &omem));
			VKCHK(vkBindBufferMemory(dev, obuf, omem, 0));
			VKCHK(vkMapMemory(dev, omem, 0, VK_WHOLE_SIZE, 0, (void **)&omap));
		}

		/* ---- import ---- */
		struct imported cur;
		VkResult r = VK_SUCCESS;
		const char *where = NULL;
		if (o.cache && cache.image && cache.bo_id == m.bo_id) {
			for (uint32_t i = 0; i < m.num_planes; i++) close(fds[i]);
			cur = cache;
			cache_hits++;
		} else {
			if (o.cache && cache.image) destroy_imported(&cache);
			r = import_dmabuf(&m, fds, &cur, &where);
		}
		int status = 0;
		if (r == VK_SUCCESS && acq_fd >= 0) {
			VkImportSemaphoreFdInfoKHR isi = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
			                                  .semaphore = acq_sem,
			                                  .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
			                                  .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
			                                  .fd = acq_fd};
			r = p_vkImportSemaphoreFdKHR(dev, &isi);
			if (r == VK_SUCCESS) {
				acq_fd = -1; /* ownership transferred */
				acq_fenced++;
			} else
				where = "vkImportSemaphoreFdKHR(SYNC_FD, TEMPORARY)";
		}
		uint64_t t1 = spike_now_ns();
		if (r != VK_SUCCESS) {
			if (acq_fd >= 0) close(acq_fd);
			fail_frames++;
			status = -1;
			if (fail_frames <= 3)
				fprintf(stderr, "consumer: frame %u IMPORT FAILED at %s: %s (%d)\n", m.frame, where,
				        spike_vk_result(r), r);
			last_fail = where, last_fail_r = r;
			struct spike_reply_msg rep = {.magic = SPIKE_MAGIC, .frame = m.frame, .status = status};
			spike_send(sock, &rep, sizeof(rep), NULL, 0);
			frames++;
			continue;
		}
		int waited_acq = (m.has_fence != 0);

		/* ---- sample ---- */
		VkDescriptorImageInfo dii = {sampler, cur.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkDescriptorBufferInfo dbi = {obuf, 0, VK_WHOLE_SIZE};
		VkWriteDescriptorSet w[2] = {
		    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
		     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii},
		    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1,
		     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi}};
		vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
		VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		                                 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
		VKCHK(vkBeginCommandBuffer(cb, &cbbi));
		if (have_ts)
			vkCmdResetQueryPool(cb, qpool, 0, 2);
		/* Acquire from the foreign (GL, other process) queue family. */
		VkImageMemoryBarrier acq = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		                            .srcAccessMask = 0,
		                            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		                            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		                            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
		                            .dstQueueFamilyIndex = qf,
		                            .image = cur.image,
		                            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
		                     0, NULL, 1, &acq);
		if (have_ts) /* after the acquire wait+barrier: measures the sampled read itself */
			vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
		int32_t pc[2] = {(int32_t)m.width, (int32_t)m.height};
		vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
		vkCmdDispatch(cb, (m.width + 15) / 16, (m.height + 15) / 16, 1);
		/* Release back to foreign so the producer may render into it again. */
		VkImageMemoryBarrier rel = acq;
		rel.srcAccessMask = VK_ACCESS_SHADER_READ_BIT, rel.dstAccessMask = 0;
		rel.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, rel.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		rel.srcQueueFamilyIndex = qf, rel.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
		VkBufferMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		                            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		                            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		                            .buffer = obuf,
		                            .size = VK_WHOLE_SIZE};
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &hb, 1,
		                     &rel);
		if (have_ts) /* after the COMPUTE->BOTTOM|HOST barrier, so the dispatch has drained */
			vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1);
		VKCHK(vkEndCommandBuffer(cb));
		VkPipelineStageFlags wst = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		                   .waitSemaphoreCount = waited_acq ? 1 : 0,
		                   .pWaitSemaphores = &acq_sem,
		                   .pWaitDstStageMask = &wst,
		                   .commandBufferCount = 1,
		                   .pCommandBuffers = &cb,
		                   .signalSemaphoreCount = rel_sem ? 1 : 0,
		                   .pSignalSemaphores = &rel_sem};
		VKCHK(vkQueueSubmit(queue, 1, &si, fence));

		/* Hand a release fence back immediately (pipelined), else ack after idle. */
		int rel_fd = -1;
		if (rel_sem) {
			VkSemaphoreGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
			                              .semaphore = rel_sem,
			                              .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
			VkResult er = p_vkGetSemaphoreFdKHR(dev, &gi, &rel_fd);
			if (er != VK_SUCCESS) {
				fprintf(stderr, "consumer: vkGetSemaphoreFdKHR(SYNC_FD) failed %s\n", spike_vk_result(er));
				rel_fd = -1;
			}
		}
		if (rel_fd < 0)
			vkQueueWaitIdle(queue);
		else
			rel_fenced++;
		struct spike_reply_msg rep = {.magic = SPIKE_MAGIC, .frame = m.frame, .has_fence = rel_fd >= 0};
		spike_send(sock, &rep, sizeof(rep), rel_fd >= 0 ? &rel_fd : NULL, rel_fd >= 0 ? 1 : 0);
		if (rel_fd >= 0) close(rel_fd);

		VKCHK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
		VKCHK(vkResetFences(dev, 1, &fence));
		uint64_t t2 = spike_now_ns();
		uint64_t gpu_ns = 0;
		if (have_ts) {
			uint64_t tsv[2];
			if (vkGetQueryPoolResults(dev, qpool, 0, 2, sizeof(tsv), tsv, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
				gpu_ns = (uint64_t)((double)(tsv[1] - tsv[0]) * ts_period_ns);
		}

		/* ---- verify ---- */
		int ha;
		spike_fourcc_to_vk(m.fourcc, &ha);
		uint64_t bad = 0;
		int step = o.sparse ? 7 : 1;
		for (uint32_t y = 0; y < m.height; y += (uint32_t)step)
			for (uint32_t x = 0; x < m.width; x += (uint32_t)step) {
				uint8_t e[4];
				spike_pattern((int)x, (int)y, (int)m.frame, (int)m.width, (int)m.height, e);
				if (!ha) e[3] = 255;
				uint32_t exp = (uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
				uint32_t got = omap[y * m.width + x];
				if (got != exp) {
					bad++;
					if (printed < 5) {
						printed++;
						fprintf(stderr, "consumer: MISMATCH frame %u (%u,%u): got %08x want %08x (ABGR-packed)\n",
						        m.frame, x, y, got, exp);
					}
				}
			}
		uint64_t t3 = spike_now_ns();
		if (bad) bad_frames++, bad_pixels += bad;

		if (o.cache)
			cache = cur;
		else
			destroy_imported(&cur);
		if (frames < MAX_FRAMES) {
			t_import[frames] = t1 - t0, t_total[frames] = t2 - t0, t_verify[frames] = t3 - t2, t_gpu[frames] = gpu_ns;
		}
		frames++;
	}
	if (o.cache && cache.image) destroy_imported(&cache);
	int fd_end = spike_count_fds();
	spike_dump_fds("consumer");

	int nf = frames < MAX_FRAMES ? frames : MAX_FRAMES;
	int ok = nf - fail_frames;
	/* failed frames have zero timings at the tail? no: they 'continue' before timing, compact: */
	qsort(t_import, (size_t)nf, 8, cmp_u64);
	qsort(t_total, (size_t)nf, 8, cmp_u64);
	qsort(t_verify, (size_t)nf, 8, cmp_u64);
	qsort(t_gpu, (size_t)nf, 8, cmp_u64);
	double ai = 0, at = 0, av = 0, ag = 0;
	for (int i = nf - ok; i < nf; i++) ai += t_import[i], at += t_total[i], av += t_verify[i], ag += t_gpu[i];
	int p99 = nf - ok + (ok * 99) / 100;
	if (p99 >= nf) p99 = nf - 1;
	printf("consumer: RESULT driver='%s' '%s' format=%s modifier=%s planes=%u planes_share_dmabuf=%s frames=%d "
	       "import_failed=%d mismatched_frames=%d mismatched_pixels=%llu verify=%s acquire_fences=%d release_fences=%d "
	       "cache=%s(hits %d) import_us avg=%.0f p99=%.0f  import+sample_us avg=%.0f p99=%.0f  gpu_sample_us avg=%.0f p99=%.0f  cpu_verify_ms avg=%.1f "
	       "fds start=%d ready=%d peak=%d end=%d%s%s\n",
	       drv.driverName, drv.driverInfo, spike_fourcc_name(first_fourcc), spike_mod_name(first_mod), first_planes,
	       g_same_inode_checked ? (g_planes_share_inode ? "yes" : "NO") : "n/a", frames, fail_frames, bad_frames,
	       (unsigned long long)bad_pixels, o.sparse ? "sparse(1/49)" : "full", acq_fenced, rel_fenced,
	       o.cache ? "on" : "off", cache_hits, ok ? ai / ok / 1e3 : 0, ok ? t_import[p99] / 1e3 : 0,
	       ok ? at / ok / 1e3 : 0, ok ? t_total[p99] / 1e3 : 0, ok ? ag / ok / 1e3 : 0, ok ? t_gpu[p99] / 1e3 : 0, ok ? av / ok / 1e6 : 0, fd_start, fd_ready, fd_peak,
	       fd_end, last_fail ? " last_failure_at=" : "", last_fail ? last_fail : "");
	if (last_fail)
		printf("consumer: last failure VkResult=%s (%d)\n", spike_vk_result(last_fail_r), last_fail_r);

	vkDeviceWaitIdle(dev);
	if (omap) vkUnmapMemory(dev, omem), vkDestroyBuffer(dev, obuf, NULL), vkFreeMemory(dev, omem, NULL);
	if (rel_sem) vkDestroySemaphore(dev, rel_sem, NULL);
	vkDestroySemaphore(dev, acq_sem, NULL);
	vkDestroyFence(dev, fence, NULL);
	vkDestroyQueryPool(dev, qpool, NULL);
	vkDestroyCommandPool(dev, cp, NULL);
	vkDestroyDescriptorPool(dev, dp, NULL);
	vkDestroySampler(dev, sampler, NULL);
	vkDestroyPipeline(dev, pipe, NULL);
	vkDestroyShaderModule(dev, sm, NULL);
	vkDestroyPipelineLayout(dev, pl, NULL);
	vkDestroyDescriptorSetLayout(dev, dsl, NULL);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	close(sock);
	return (fail_frames || bad_frames) ? 3 : 0;
}
