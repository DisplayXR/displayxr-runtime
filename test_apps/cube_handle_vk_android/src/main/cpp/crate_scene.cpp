// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// crate_scene implementation (#499). Ported from cube_handle_vk_win's
// vk_renderer.cpp: a Wood_Crate textured cube (basecolor + normal + AO,
// normal-mapped directional lighting) resting on a line-grid floor. Shaders are
// the Windows app's embedded SPIR-V (portable, host-endian); textures load from
// the APK assets via AAssetManager + stb_image.

#include "crate_scene.h"

#include <android/log.h>
#include <cstddef>
#include <cstring>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Embedded SPIR-V: g_cubeTexturedVertSpv / g_cubeTexturedFragSpv /
// g_gridVertSpv / g_gridFragSpv.
#include "crate_scene_spv.inc"

#define LOG_TAG "cube_handle_vk_android"
#define SLOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "crate_scene: " __VA_ARGS__)
#define SLOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "crate_scene: " __VA_ARGS__)

namespace {

// Vertex layouts (match the cube_handle_vk_win pipelines).
struct SceneVertex
{
	float pos[3];
	float color[4];
	float uv[2];
	float normal[3];
	float tangent[3];
};
struct GridVertex
{
	float pos[3];
};

// Push constants.
struct CubePush
{
	float mvp[16];
	float model[16];
};
struct GridPush
{
	float transform[16];
	float color[4];
};

uint32_t
find_memory_type(VkPhysicalDevice phys, uint32_t type_filter, VkMemoryPropertyFlags props)
{
	VkPhysicalDeviceMemoryProperties mem = {};
	vkGetPhysicalDeviceMemoryProperties(phys, &mem);
	for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
		if ((type_filter & (1u << i)) &&
		    (mem.memoryTypes[i].propertyFlags & props) == props) {
			return i;
		}
	}
	return UINT32_MAX;
}

bool
create_buffer(VkDevice device, VkPhysicalDevice phys, VkDeviceSize size, VkBufferUsageFlags usage,
              VkMemoryPropertyFlags props, VkBuffer &buf, VkDeviceMemory &mem)
{
	VkBufferCreateInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bi.size = size;
	bi.usage = usage;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(device, &bi, nullptr, &buf) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements req = {};
	vkGetBufferMemoryRequirements(device, buf, &req);
	VkMemoryAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = find_memory_type(phys, req.memoryTypeBits, props);
	if (ai.memoryTypeIndex == UINT32_MAX ||
	    vkAllocateMemory(device, &ai, nullptr, &mem) != VK_SUCCESS) {
		return false;
	}
	vkBindBufferMemory(device, buf, mem, 0);
	return true;
}

VkShaderModule
create_shader_module(VkDevice device, const uint32_t *code, size_t size_bytes)
{
	VkShaderModuleCreateInfo ci = {};
	ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	ci.codeSize = size_bytes;
	ci.pCode = code;
	VkShaderModule m = VK_NULL_HANDLE;
	if (vkCreateShaderModule(device, &ci, nullptr, &m) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	return m;
}

// Load an RGBA image from the APK assets. Returns stbi-allocated pixels (free
// with stbi_image_free) or nullptr.
unsigned char *
load_asset_rgba(AAssetManager *am, const char *name, int *w, int *h)
{
	if (am == nullptr) {
		return nullptr;
	}
	AAsset *a = AAssetManager_open(am, name, AASSET_MODE_BUFFER);
	if (a == nullptr) {
		return nullptr;
	}
	const off_t len = AAsset_getLength(a);
	const void *buf = AAsset_getBuffer(a);
	unsigned char *px = nullptr;
	if (buf != nullptr && len > 0) {
		int ch = 0;
		px = stbi_load_from_memory(static_cast<const stbi_uc *>(buf), static_cast<int>(len), w, h,
		                           &ch, 4);
	}
	AAsset_close(a);
	return px;
}

// One graphics pipeline. `line` selects LINE_LIST + no cull (grid) vs
// TRIANGLE_LIST (cube). Depth test on. Viewport/scissor dynamic.
VkPipeline
build_pipeline(VkDevice device, VkRenderPass rp, VkPipelineLayout layout, VkShaderModule vert,
               VkShaderModule frag, const VkVertexInputBindingDescription &binding,
               const VkVertexInputAttributeDescription *attrs, uint32_t attr_count, bool line)
{
	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag;
	stages[1].pName = "main";

	VkPipelineVertexInputStateCreateInfo vi = {};
	vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &binding;
	vi.vertexAttributeDescriptionCount = attr_count;
	vi.pVertexAttributeDescriptions = attrs;

	VkPipelineInputAssemblyStateCreateInfo ia = {};
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.topology = line ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo vp = {};
	vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	vp.viewportCount = 1;
	vp.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rs = {};
	rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.lineWidth = 1.0f;
	// No cull: the host bakes the Vulkan Y-flip into the projection matrix
	// (not a negative-height viewport), so triangle winding as seen by the
	// rasterizer differs from the desktop app — depth testing makes occlusion
	// correct regardless, so drawing both faces is the safe, correct choice.
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

	VkPipelineMultisampleStateCreateInfo ms = {};
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo ds = {};
	ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = VK_TRUE;
	ds.depthCompareOp = VK_COMPARE_OP_LESS;

	VkPipelineColorBlendAttachmentState cba = {};
	cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo cb = {};
	cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;

	VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dyn = {};
	dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dyn.dynamicStateCount = 2;
	dyn.pDynamicStates = dyn_states;

	VkGraphicsPipelineCreateInfo pi = {};
	pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pi.stageCount = 2;
	pi.pStages = stages;
	pi.pVertexInputState = &vi;
	pi.pInputAssemblyState = &ia;
	pi.pViewportState = &vp;
	pi.pRasterizationState = &rs;
	pi.pMultisampleState = &ms;
	pi.pDepthStencilState = &ds;
	pi.pColorBlendState = &cb;
	pi.pDynamicState = &dyn;
	pi.layout = layout;
	pi.renderPass = rp;
	pi.subpass = 0;

	VkPipeline pipe = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipe) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	return pipe;
}

bool
upload_textures(CrateScene &s, AAssetManager *assets)
{
	static const char *kFiles[3] = {
	    "Wood_Crate_001_basecolor.jpg",
	    "Wood_Crate_001_normal.jpg",
	    "Wood_Crate_001_ambientOcclusion.jpg",
	};
	const unsigned char white_px[4] = {255, 255, 255, 255};
	const unsigned char normal_px[4] = {128, 128, 255, 255};

	// A throwaway command buffer for the staging copies + mip blits.
	VkCommandBufferAllocateInfo cba = {};
	cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cba.commandPool = s.upload_pool;
	cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cba.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(s.device, &cba, &cmd) != VK_SUCCESS) {
		return false;
	}

	for (int i = 0; i < 3; ++i) {
		int w = 0, h = 0;
		unsigned char *pixels = load_asset_rgba(assets, kFiles[i], &w, &h);
		const unsigned char *src;
		if (pixels != nullptr) {
			src = pixels;
			SLOGI("loaded %s (%dx%d)", kFiles[i], w, h);
		} else {
			w = 1;
			h = 1;
			src = (i == 1) ? normal_px : white_px;
			SLOGI("fallback texture for %s", kFiles[i]);
		}

		uint32_t mip_levels = 1;
		if (pixels != nullptr) {
			uint32_t max_dim = (uint32_t)((w > h) ? w : h);
			while (max_dim > 1) {
				max_dim >>= 1;
				mip_levels++;
			}
		}

		VkImageCreateInfo ici = {};
		ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = VK_FORMAT_R8G8B8A8_UNORM;
		ici.extent = {(uint32_t)w, (uint32_t)h, 1};
		ici.mipLevels = mip_levels;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
		            VK_IMAGE_USAGE_SAMPLED_BIT;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(s.device, &ici, nullptr, &s.tex_img[i]) != VK_SUCCESS) {
			if (pixels) stbi_image_free(pixels);
			return false;
		}
		VkMemoryRequirements req = {};
		vkGetImageMemoryRequirements(s.device, s.tex_img[i], &req);
		VkMemoryAllocateInfo mai = {};
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = find_memory_type(s.phys, req.memoryTypeBits,
		                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (mai.memoryTypeIndex == UINT32_MAX ||
		    vkAllocateMemory(s.device, &mai, nullptr, &s.tex_mem[i]) != VK_SUCCESS) {
			if (pixels) stbi_image_free(pixels);
			return false;
		}
		vkBindImageMemory(s.device, s.tex_img[i], s.tex_mem[i], 0);

		VkDeviceSize image_size = (VkDeviceSize)w * h * 4;
		VkBuffer staging = VK_NULL_HANDLE;
		VkDeviceMemory staging_mem = VK_NULL_HANDLE;
		create_buffer(s.device, s.phys, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		              staging, staging_mem);
		void *mapped = nullptr;
		vkMapMemory(s.device, staging_mem, 0, image_size, 0, &mapped);
		memcpy(mapped, src, (size_t)image_size);
		vkUnmapMemory(s.device, staging_mem);
		if (pixels) stbi_image_free(pixels);

		VkCommandBufferBeginInfo bi = {};
		bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkResetCommandBuffer(cmd, 0);
		vkBeginCommandBuffer(cmd, &bi);

		VkImageMemoryBarrier b = {};
		b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = s.tex_img[i];
		b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		b.subresourceRange.levelCount = mip_levels;
		b.subresourceRange.layerCount = 1;
		b.srcAccessMask = 0;
		b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     0, 0, nullptr, 0, nullptr, 1, &b);

		VkBufferImageCopy region = {};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
		vkCmdCopyBufferToImage(cmd, staging, s.tex_img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
		                       &region);

		// Generate the mip chain: blit each level down to the next.
		b.subresourceRange.levelCount = 1;
		int32_t mip_w = w, mip_h = h;
		for (uint32_t level = 1; level < mip_levels; ++level) {
			b.subresourceRange.baseMipLevel = level - 1;
			b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			                     0, 0, nullptr, 0, nullptr, 1, &b);

			VkImageBlit blit = {};
			blit.srcOffsets[1] = {mip_w, mip_h, 1};
			blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			blit.srcSubresource.mipLevel = level - 1;
			blit.srcSubresource.layerCount = 1;
			blit.dstOffsets[1] = {mip_w > 1 ? mip_w / 2 : 1, mip_h > 1 ? mip_h / 2 : 1, 1};
			blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			blit.dstSubresource.mipLevel = level;
			blit.dstSubresource.layerCount = 1;
			vkCmdBlitImage(cmd, s.tex_img[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.tex_img[i],
			               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

			b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
			                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
			                     &b);

			if (mip_w > 1) mip_w /= 2;
			if (mip_h > 1) mip_h /= 2;
		}

		// Last level: TRANSFER_DST -> SHADER_READ.
		b.subresourceRange.baseMipLevel = mip_levels - 1;
		b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

		vkEndCommandBuffer(cmd);
		VkSubmitInfo si = {};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cmd;
		vkQueueSubmit(s.queue, 1, &si, VK_NULL_HANDLE);
		vkQueueWaitIdle(s.queue);

		vkDestroyBuffer(s.device, staging, nullptr);
		vkFreeMemory(s.device, staging_mem, nullptr);

		VkImageViewCreateInfo vci = {};
		vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vci.image = s.tex_img[i];
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = VK_FORMAT_R8G8B8A8_UNORM;
		vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vci.subresourceRange.levelCount = mip_levels;
		vci.subresourceRange.layerCount = 1;
		vkCreateImageView(s.device, &vci, nullptr, &s.tex_view[i]);
	}

	vkFreeCommandBuffers(s.device, s.upload_pool, 1, &cmd);

	VkSamplerCreateInfo sci = {};
	sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.maxLod = VK_LOD_CLAMP_NONE;
	vkCreateSampler(s.device, &sci, nullptr, &s.sampler);

	VkDescriptorPoolSize ps = {};
	ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	ps.descriptorCount = 3;
	VkDescriptorPoolCreateInfo pci = {};
	pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pci.maxSets = 1;
	pci.poolSizeCount = 1;
	pci.pPoolSizes = &ps;
	vkCreateDescriptorPool(s.device, &pci, nullptr, &s.desc_pool);

	VkDescriptorSetAllocateInfo dsa = {};
	dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsa.descriptorPool = s.desc_pool;
	dsa.descriptorSetCount = 1;
	dsa.pSetLayouts = &s.desc_set_layout;
	if (vkAllocateDescriptorSets(s.device, &dsa, &s.desc_set) != VK_SUCCESS) {
		return false;
	}

	VkDescriptorImageInfo infos[3] = {};
	VkWriteDescriptorSet writes[3] = {};
	for (int i = 0; i < 3; ++i) {
		infos[i].sampler = s.sampler;
		infos[i].imageView = s.tex_view[i];
		infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = s.desc_set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &infos[i];
	}
	vkUpdateDescriptorSets(s.device, 3, writes, 0, nullptr);
	s.textures_loaded = true;
	return true;
}

} // namespace

bool
crate_scene_init(CrateScene &s, VkDevice device, VkPhysicalDevice phys, VkQueue queue,
                 uint32_t queue_family, VkRenderPass render_pass, AAssetManager *assets)
{
	s.device = device;
	s.phys = phys;
	s.queue = queue;
	s.queue_family = queue_family;

	// Upload command pool.
	VkCommandPoolCreateInfo pci = {};
	pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = queue_family;
	if (vkCreateCommandPool(device, &pci, nullptr, &s.upload_pool) != VK_SUCCESS) {
		SLOGE("upload command pool failed");
		return false;
	}

	// Descriptor set layout: 3 combined image samplers in the fragment stage.
	{
		VkDescriptorSetLayoutBinding bindings[3] = {};
		for (int i = 0; i < 3; ++i) {
			bindings[i].binding = i;
			bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			bindings[i].descriptorCount = 1;
			bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		}
		VkDescriptorSetLayoutCreateInfo li = {};
		li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		li.bindingCount = 3;
		li.pBindings = bindings;
		if (vkCreateDescriptorSetLayout(device, &li, nullptr, &s.desc_set_layout) != VK_SUCCESS) {
			SLOGE("descriptor set layout failed");
			return false;
		}
	}

	// Cube pipeline layout: descriptor set + 128-byte vertex push.
	{
		VkPushConstantRange pr = {};
		pr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		pr.offset = 0;
		pr.size = sizeof(CubePush);
		VkPipelineLayoutCreateInfo li = {};
		li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		li.setLayoutCount = 1;
		li.pSetLayouts = &s.desc_set_layout;
		li.pushConstantRangeCount = 1;
		li.pPushConstantRanges = &pr;
		if (vkCreatePipelineLayout(device, &li, nullptr, &s.cube_layout) != VK_SUCCESS) {
			SLOGE("cube pipeline layout failed");
			return false;
		}
	}

	// Grid pipeline layout: 80-byte push (transform + color), vertex+fragment.
	{
		VkPushConstantRange pr = {};
		pr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		pr.offset = 0;
		pr.size = sizeof(GridPush);
		VkPipelineLayoutCreateInfo li = {};
		li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		li.pushConstantRangeCount = 1;
		li.pPushConstantRanges = &pr;
		if (vkCreatePipelineLayout(device, &li, nullptr, &s.grid_layout) != VK_SUCCESS) {
			SLOGE("grid pipeline layout failed");
			return false;
		}
	}

	VkShaderModule cube_vert = create_shader_module(device, g_cubeTexturedVertSpv,
	                                                sizeof(g_cubeTexturedVertSpv));
	VkShaderModule cube_frag = create_shader_module(device, g_cubeTexturedFragSpv,
	                                                sizeof(g_cubeTexturedFragSpv));
	VkShaderModule grid_vert = create_shader_module(device, g_gridVertSpv, sizeof(g_gridVertSpv));
	VkShaderModule grid_frag = create_shader_module(device, g_gridFragSpv, sizeof(g_gridFragSpv));
	if (!cube_vert || !cube_frag || !grid_vert || !grid_frag) {
		SLOGE("shader module creation failed");
		return false;
	}

	// Cube pipeline (5 attrs: pos/color/uv/normal/tangent).
	{
		VkVertexInputBindingDescription binding = {};
		binding.binding = 0;
		binding.stride = sizeof(SceneVertex);
		binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		VkVertexInputAttributeDescription attrs[5] = {};
		attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SceneVertex, pos)};
		attrs[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SceneVertex, color)};
		attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SceneVertex, uv)};
		attrs[3] = {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SceneVertex, normal)};
		attrs[4] = {4, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SceneVertex, tangent)};
		s.cube_pipeline = build_pipeline(device, render_pass, s.cube_layout, cube_vert, cube_frag,
		                                 binding, attrs, 5, /*line=*/false);
	}
	// Grid pipeline (1 attr: pos).
	{
		VkVertexInputBindingDescription binding = {};
		binding.binding = 0;
		binding.stride = sizeof(GridVertex);
		binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		VkVertexInputAttributeDescription attr = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
		s.grid_pipeline = build_pipeline(device, render_pass, s.grid_layout, grid_vert, grid_frag,
		                                 binding, &attr, 1, /*line=*/true);
	}

	vkDestroyShaderModule(device, cube_vert, nullptr);
	vkDestroyShaderModule(device, cube_frag, nullptr);
	vkDestroyShaderModule(device, grid_vert, nullptr);
	vkDestroyShaderModule(device, grid_frag, nullptr);
	if (!s.cube_pipeline || !s.grid_pipeline) {
		SLOGE("pipeline creation failed");
		return false;
	}

	// Cube geometry: unit cube (±0.5) with per-face uv/normal/tangent. Vertex
	// color is white — the crate texture supplies the color.
	static const SceneVertex kCube[] = {
	    // Front (-Z)
	    {{-0.5f, -0.5f, -0.5f}, {1, 1, 1, 1}, {0, 1}, {0, 0, -1}, {1, 0, 0}},
	    {{-0.5f, 0.5f, -0.5f}, {1, 1, 1, 1}, {0, 0}, {0, 0, -1}, {1, 0, 0}},
	    {{0.5f, 0.5f, -0.5f}, {1, 1, 1, 1}, {1, 0}, {0, 0, -1}, {1, 0, 0}},
	    {{0.5f, -0.5f, -0.5f}, {1, 1, 1, 1}, {1, 1}, {0, 0, -1}, {1, 0, 0}},
	    // Back (+Z)
	    {{-0.5f, -0.5f, 0.5f}, {1, 1, 1, 1}, {1, 1}, {0, 0, 1}, {-1, 0, 0}},
	    {{0.5f, -0.5f, 0.5f}, {1, 1, 1, 1}, {0, 1}, {0, 0, 1}, {-1, 0, 0}},
	    {{0.5f, 0.5f, 0.5f}, {1, 1, 1, 1}, {0, 0}, {0, 0, 1}, {-1, 0, 0}},
	    {{-0.5f, 0.5f, 0.5f}, {1, 1, 1, 1}, {1, 0}, {0, 0, 1}, {-1, 0, 0}},
	    // Top (+Y)
	    {{-0.5f, 0.5f, -0.5f}, {1, 1, 1, 1}, {0, 1}, {0, 1, 0}, {1, 0, 0}},
	    {{-0.5f, 0.5f, 0.5f}, {1, 1, 1, 1}, {0, 0}, {0, 1, 0}, {1, 0, 0}},
	    {{0.5f, 0.5f, 0.5f}, {1, 1, 1, 1}, {1, 0}, {0, 1, 0}, {1, 0, 0}},
	    {{0.5f, 0.5f, -0.5f}, {1, 1, 1, 1}, {1, 1}, {0, 1, 0}, {1, 0, 0}},
	    // Bottom (-Y)
	    {{-0.5f, -0.5f, -0.5f}, {1, 1, 1, 1}, {0, 0}, {0, -1, 0}, {1, 0, 0}},
	    {{0.5f, -0.5f, -0.5f}, {1, 1, 1, 1}, {1, 0}, {0, -1, 0}, {1, 0, 0}},
	    {{0.5f, -0.5f, 0.5f}, {1, 1, 1, 1}, {1, 1}, {0, -1, 0}, {1, 0, 0}},
	    {{-0.5f, -0.5f, 0.5f}, {1, 1, 1, 1}, {0, 1}, {0, -1, 0}, {1, 0, 0}},
	    // Left (-X)
	    {{-0.5f, -0.5f, 0.5f}, {1, 1, 1, 1}, {0, 1}, {-1, 0, 0}, {0, 0, -1}},
	    {{-0.5f, 0.5f, 0.5f}, {1, 1, 1, 1}, {0, 0}, {-1, 0, 0}, {0, 0, -1}},
	    {{-0.5f, 0.5f, -0.5f}, {1, 1, 1, 1}, {1, 0}, {-1, 0, 0}, {0, 0, -1}},
	    {{-0.5f, -0.5f, -0.5f}, {1, 1, 1, 1}, {1, 1}, {-1, 0, 0}, {0, 0, -1}},
	    // Right (+X)
	    {{0.5f, -0.5f, -0.5f}, {1, 1, 1, 1}, {0, 1}, {1, 0, 0}, {0, 0, 1}},
	    {{0.5f, 0.5f, -0.5f}, {1, 1, 1, 1}, {0, 0}, {1, 0, 0}, {0, 0, 1}},
	    {{0.5f, 0.5f, 0.5f}, {1, 1, 1, 1}, {1, 0}, {1, 0, 0}, {0, 0, 1}},
	    {{0.5f, -0.5f, 0.5f}, {1, 1, 1, 1}, {1, 1}, {1, 0, 0}, {0, 0, 1}},
	};
	static const uint16_t kIndices[] = {
	    0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
	    12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
	};

	if (!create_buffer(device, phys, sizeof(kCube), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                   s.cube_vbuf, s.cube_vmem)) {
		return false;
	}
	void *data = nullptr;
	vkMapMemory(device, s.cube_vmem, 0, sizeof(kCube), 0, &data);
	memcpy(data, kCube, sizeof(kCube));
	vkUnmapMemory(device, s.cube_vmem);

	if (!create_buffer(device, phys, sizeof(kIndices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
	                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                   s.cube_ibuf, s.cube_imem)) {
		return false;
	}
	vkMapMemory(device, s.cube_imem, 0, sizeof(kIndices), 0, &data);
	memcpy(data, kIndices, sizeof(kIndices));
	vkUnmapMemory(device, s.cube_imem);

	// Grid geometry: a 21x21 line grid at y=-1 (placed at the floor by the
	// host's grid model matrix).
	{
		const int grid_size = 10;
		const float spacing = 1.0f;
		std::vector<GridVertex> verts;
		for (int i = -grid_size; i <= grid_size; ++i) {
			verts.push_back({{(float)i * spacing, -1.0f, -grid_size * spacing}});
			verts.push_back({{(float)i * spacing, -1.0f, grid_size * spacing}});
			verts.push_back({{-grid_size * spacing, -1.0f, (float)i * spacing}});
			verts.push_back({{grid_size * spacing, -1.0f, (float)i * spacing}});
		}
		s.grid_vertex_count = (uint32_t)verts.size();
		VkDeviceSize size = verts.size() * sizeof(GridVertex);
		if (!create_buffer(device, phys, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		                   s.grid_vbuf, s.grid_vmem)) {
			return false;
		}
		vkMapMemory(device, s.grid_vmem, 0, size, 0, &data);
		memcpy(data, verts.data(), (size_t)size);
		vkUnmapMemory(device, s.grid_vmem);
	}

	if (!upload_textures(s, assets)) {
		SLOGE("texture upload failed (continuing with whatever loaded)");
	}

	SLOGI("init complete (textures=%s, grid_verts=%u)", s.textures_loaded ? "yes" : "no",
	      s.grid_vertex_count);
	return true;
}

void
crate_scene_draw_cube(const CrateScene &s, VkCommandBuffer cmd, const float mvp[16],
                      const float model[16])
{
	CubePush pc = {};
	memcpy(pc.mvp, mvp, sizeof(pc.mvp));
	memcpy(pc.model, model, sizeof(pc.model));
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.cube_pipeline);
	vkCmdPushConstants(cmd, s.cube_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
	if (s.textures_loaded) {
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.cube_layout, 0, 1,
		                        &s.desc_set, 0, nullptr);
	}
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &s.cube_vbuf, &offset);
	vkCmdBindIndexBuffer(cmd, s.cube_ibuf, 0, VK_INDEX_TYPE_UINT16);
	vkCmdDrawIndexed(cmd, 36, 1, 0, 0, 0);
}

void
crate_scene_draw_grid(const CrateScene &s, VkCommandBuffer cmd, const float mvp[16],
                      const float color[4])
{
	GridPush pc = {};
	memcpy(pc.transform, mvp, sizeof(pc.transform));
	memcpy(pc.color, color, sizeof(pc.color));
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.grid_pipeline);
	vkCmdPushConstants(cmd, s.grid_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
	                   0, sizeof(pc), &pc);
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &s.grid_vbuf, &offset);
	vkCmdDraw(cmd, s.grid_vertex_count, 1, 0, 0);
}

void
crate_scene_destroy(CrateScene &s)
{
	if (s.device == VK_NULL_HANDLE) {
		return;
	}
	vkDeviceWaitIdle(s.device);
	if (s.desc_pool) vkDestroyDescriptorPool(s.device, s.desc_pool, nullptr);
	if (s.sampler) vkDestroySampler(s.device, s.sampler, nullptr);
	for (int i = 0; i < 3; ++i) {
		if (s.tex_view[i]) vkDestroyImageView(s.device, s.tex_view[i], nullptr);
		if (s.tex_img[i]) vkDestroyImage(s.device, s.tex_img[i], nullptr);
		if (s.tex_mem[i]) vkFreeMemory(s.device, s.tex_mem[i], nullptr);
	}
	if (s.grid_vbuf) vkDestroyBuffer(s.device, s.grid_vbuf, nullptr);
	if (s.grid_vmem) vkFreeMemory(s.device, s.grid_vmem, nullptr);
	if (s.cube_ibuf) vkDestroyBuffer(s.device, s.cube_ibuf, nullptr);
	if (s.cube_imem) vkFreeMemory(s.device, s.cube_imem, nullptr);
	if (s.cube_vbuf) vkDestroyBuffer(s.device, s.cube_vbuf, nullptr);
	if (s.cube_vmem) vkFreeMemory(s.device, s.cube_vmem, nullptr);
	if (s.cube_pipeline) vkDestroyPipeline(s.device, s.cube_pipeline, nullptr);
	if (s.grid_pipeline) vkDestroyPipeline(s.device, s.grid_pipeline, nullptr);
	if (s.cube_layout) vkDestroyPipelineLayout(s.device, s.cube_layout, nullptr);
	if (s.grid_layout) vkDestroyPipelineLayout(s.device, s.grid_layout, nullptr);
	if (s.desc_set_layout) vkDestroyDescriptorSetLayout(s.device, s.desc_set_layout, nullptr);
	if (s.upload_pool) vkDestroyCommandPool(s.device, s.upload_pool, nullptr);
	s = CrateScene{};
}
