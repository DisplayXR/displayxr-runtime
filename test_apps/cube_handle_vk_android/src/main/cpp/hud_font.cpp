// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// hud_font implementation — see hud_font.h. stb_truetype bakes a /system/fonts
// TTF into an R8 coverage atlas; we draw textured glyph quads that sample it.

#include "hud_font.h"

#include <android/log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "hud_font_spv.inc"

#define HF_TAG "cube_handle_vk_android"
#define HFLOGE(...) __android_log_print(ANDROID_LOG_ERROR, HF_TAG, __VA_ARGS__)
#define HFLOGI(...) __android_log_print(ANDROID_LOG_INFO, HF_TAG, __VA_ARGS__)

namespace {

struct TextVtx {
	float pos[2];
	float uv[2];
};
constexpr uint32_t kMaxVerts = HUD_FONT_MAX_QUADS * 6;

uint32_t
find_mem_type(VkPhysicalDevice phys, uint32_t filter, VkMemoryPropertyFlags props)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(phys, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
		if ((filter & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
			return i;
		}
	}
	return 0;
}

bool
make_buffer(VkPhysicalDevice phys, VkDevice dev, VkDeviceSize size, VkBufferUsageFlags usage,
            VkMemoryPropertyFlags props, VkBuffer &buf, VkDeviceMemory &mem)
{
	VkBufferCreateInfo bci = {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = size;
	bci.usage = usage;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(dev, &bci, nullptr, &buf) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, buf, &req);
	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = find_mem_type(phys, req.memoryTypeBits, props);
	if (vkAllocateMemory(dev, &mai, nullptr, &mem) != VK_SUCCESS) {
		return false;
	}
	vkBindBufferMemory(dev, buf, mem, 0);
	return true;
}

VkShaderModule
make_shader(VkDevice dev, const uint32_t *code, size_t bytes)
{
	VkShaderModuleCreateInfo ci = {};
	ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	ci.codeSize = bytes;
	ci.pCode = code;
	VkShaderModule m = VK_NULL_HANDLE;
	vkCreateShaderModule(dev, &ci, nullptr, &m);
	return m;
}

// Read an entire file into a malloc'd buffer. Returns nullptr on failure.
uint8_t *
read_file(const char *path, long *out_size)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		return nullptr;
	}
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz <= 0) {
		fclose(fp);
		return nullptr;
	}
	uint8_t *buf = (uint8_t *)malloc((size_t)sz);
	if (buf && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
		free(buf);
		buf = nullptr;
	}
	fclose(fp);
	if (out_size) {
		*out_size = sz;
	}
	return buf;
}

} // namespace

bool
hud_font_init(HudFont &f, VkPhysicalDevice phys, VkDevice device, VkQueue queue,
              uint32_t queue_family, VkRenderPass rp, float pixel_height)
{
	f.device = device;
	f.pixel_height = pixel_height;
	f.atlas_w = 512;
	f.atlas_h = 256;

	// ── 1. Load a system TTF and bake the ASCII glyph atlas (R8 coverage). ──
	const char *candidates[] = {
	    "/system/fonts/DroidSansMono.ttf",  // monospace — tidy columns for a readout
	    "/system/fonts/RobotoMono-Regular.ttf",
	    "/system/fonts/Roboto-Regular.ttf",
	    "/system/fonts/DroidSans.ttf",
	};
	uint8_t *ttf = nullptr;
	for (const char *p : candidates) {
		ttf = read_file(p, nullptr);
		if (ttf) {
			HFLOGI("hud_font: loaded %s", p);
			break;
		}
	}
	if (!ttf) {
		HFLOGE("hud_font: no system TTF found; falling back to bitmap HUD");
		return false;
	}

	std::vector<uint8_t> atlas((size_t)f.atlas_w * f.atlas_h, 0);
	stbtt_bakedchar baked[HUD_FONT_NUM_CHARS];
	int baked_rows = stbtt_BakeFontBitmap(ttf, 0, pixel_height, atlas.data(), (int)f.atlas_w,
	                                      (int)f.atlas_h, HUD_FONT_FIRST_CHAR, HUD_FONT_NUM_CHARS,
	                                      baked);
	free(ttf);
	if (baked_rows <= 0) {
		HFLOGE("hud_font: stbtt_BakeFontBitmap failed (atlas too small?)");
		return false;
	}
	static_assert(sizeof(HudFontBakedChar) == sizeof(stbtt_bakedchar), "bakedchar layout");
	memcpy(f.chars, baked, sizeof(baked));

	// ── 2. R8 atlas texture + staging upload + transition to SHADER_READ. ──
	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R8_UNORM;
	ici.extent = {f.atlas_w, f.atlas_h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(device, &ici, nullptr, &f.atlas_img) != VK_SUCCESS) {
		HFLOGE("hud_font: vkCreateImage failed");
		return false;
	}
	VkMemoryRequirements ireq;
	vkGetImageMemoryRequirements(device, f.atlas_img, &ireq);
	VkMemoryAllocateInfo iai = {};
	iai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	iai.allocationSize = ireq.size;
	iai.memoryTypeIndex = find_mem_type(phys, ireq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (vkAllocateMemory(device, &iai, nullptr, &f.atlas_mem) != VK_SUCCESS) {
		HFLOGE("hud_font: image memory alloc failed");
		return false;
	}
	vkBindImageMemory(device, f.atlas_img, f.atlas_mem, 0);

	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory staging_mem = VK_NULL_HANDLE;
	const VkDeviceSize atlas_bytes = (VkDeviceSize)f.atlas_w * f.atlas_h;
	if (!make_buffer(phys, device, atlas_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                 staging, staging_mem)) {
		HFLOGE("hud_font: staging buffer failed");
		return false;
	}
	void *sp = nullptr;
	vkMapMemory(device, staging_mem, 0, atlas_bytes, 0, &sp);
	memcpy(sp, atlas.data(), (size_t)atlas_bytes);
	vkUnmapMemory(device, staging_mem);

	// One-shot command buffer for the copy + layout transitions.
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandPoolCreateInfo pci = {};
	pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pci.queueFamilyIndex = queue_family;
	vkCreateCommandPool(device, &pci, nullptr, &pool);
	VkCommandBufferAllocateInfo cbai = {};
	cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandPool = pool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	vkAllocateCommandBuffers(device, &cbai, &cmd);
	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);

	VkImageMemoryBarrier b = {};
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = f.atlas_img;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	b.srcAccessMask = 0;
	b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
	                     nullptr, 0, nullptr, 1, &b);

	VkBufferImageCopy region = {};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {f.atlas_w, f.atlas_h, 1};
	vkCmdCopyBufferToImage(cmd, staging, f.atlas_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
	                     0, nullptr, 0, nullptr, 1, &b);

	vkEndCommandBuffer(cmd);
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
	vkQueueWaitIdle(queue);
	vkFreeCommandBuffers(device, pool, 1, &cmd);
	vkDestroyCommandPool(device, pool, nullptr);
	vkDestroyBuffer(device, staging, nullptr);
	vkFreeMemory(device, staging_mem, nullptr);

	// View + sampler.
	VkImageViewCreateInfo vci = {};
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = f.atlas_img;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = VK_FORMAT_R8_UNORM;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (vkCreateImageView(device, &vci, nullptr, &f.atlas_view) != VK_SUCCESS) {
		HFLOGE("hud_font: image view failed");
		return false;
	}
	VkSamplerCreateInfo sci = {};
	sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	if (vkCreateSampler(device, &sci, nullptr, &f.sampler) != VK_SUCCESS) {
		HFLOGE("hud_font: sampler failed");
		return false;
	}

	// ── 3. Descriptor set (1 combined image sampler). ──
	VkDescriptorSetLayoutBinding dlb = {};
	dlb.binding = 0;
	dlb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	dlb.descriptorCount = 1;
	dlb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo dli = {};
	dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dli.bindingCount = 1;
	dli.pBindings = &dlb;
	if (vkCreateDescriptorSetLayout(device, &dli, nullptr, &f.dset_layout) != VK_SUCCESS) {
		HFLOGE("hud_font: dset layout failed");
		return false;
	}
	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
	VkDescriptorPoolCreateInfo dpi = {};
	dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpi.maxSets = 1;
	dpi.poolSizeCount = 1;
	dpi.pPoolSizes = &ps;
	if (vkCreateDescriptorPool(device, &dpi, nullptr, &f.dpool) != VK_SUCCESS) {
		HFLOGE("hud_font: dpool failed");
		return false;
	}
	VkDescriptorSetAllocateInfo dai = {};
	dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dai.descriptorPool = f.dpool;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &f.dset_layout;
	if (vkAllocateDescriptorSets(device, &dai, &f.dset) != VK_SUCCESS) {
		HFLOGE("hud_font: dset alloc failed");
		return false;
	}
	VkDescriptorImageInfo dii = {};
	dii.sampler = f.sampler;
	dii.imageView = f.atlas_view;
	dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkWriteDescriptorSet w = {};
	w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	w.dstSet = f.dset;
	w.dstBinding = 0;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = &dii;
	vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);

	// ── 4. Pipeline (textured-glyph, alpha blend, no depth). ──
	VkPushConstantRange pcr = {};
	pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	pcr.offset = 0;
	pcr.size = 80; // mat4 (64) + vec4 (16)
	VkPipelineLayoutCreateInfo pli = {};
	pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &f.dset_layout;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pcr;
	if (vkCreatePipelineLayout(device, &pli, nullptr, &f.pipe_layout) != VK_SUCCESS) {
		HFLOGE("hud_font: pipeline layout failed");
		return false;
	}

	VkShaderModule vert = make_shader(device, g_hudTextVertSpv, sizeof(g_hudTextVertSpv));
	VkShaderModule frag = make_shader(device, g_hudTextFragSpv, sizeof(g_hudTextFragSpv));
	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag;
	stages[1].pName = "main";

	VkVertexInputBindingDescription bind = {0, sizeof(TextVtx), VK_VERTEX_INPUT_RATE_VERTEX};
	VkVertexInputAttributeDescription attrs[2] = {};
	attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TextVtx, pos)};
	attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TextVtx, uv)};
	VkPipelineVertexInputStateCreateInfo vis = {};
	vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vis.vertexBindingDescriptionCount = 1;
	vis.pVertexBindingDescriptions = &bind;
	vis.vertexAttributeDescriptionCount = 2;
	vis.pVertexAttributeDescriptions = attrs;

	VkPipelineInputAssemblyStateCreateInfo ia = {};
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo vp = {};
	vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	vp.viewportCount = 1;
	vp.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rs = {};
	rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo ms = {};
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineColorBlendAttachmentState cba = {};
	cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	cba.blendEnable = VK_TRUE;
	cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	cba.colorBlendOp = VK_BLEND_OP_ADD;
	cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	cba.alphaBlendOp = VK_BLEND_OP_ADD;
	VkPipelineColorBlendStateCreateInfo cb = {};
	cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;

	VkPipelineDepthStencilStateCreateInfo ds = {};
	ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	ds.depthTestEnable = VK_FALSE;  // HUD text always on top
	ds.depthWriteEnable = VK_FALSE;

	const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dyn = {};
	dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dyn.dynamicStateCount = 2;
	dyn.pDynamicStates = dyn_states;

	VkGraphicsPipelineCreateInfo gpi = {};
	gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	gpi.stageCount = 2;
	gpi.pStages = stages;
	gpi.pVertexInputState = &vis;
	gpi.pInputAssemblyState = &ia;
	gpi.pViewportState = &vp;
	gpi.pRasterizationState = &rs;
	gpi.pMultisampleState = &ms;
	gpi.pColorBlendState = &cb;
	gpi.pDepthStencilState = &ds;
	gpi.pDynamicState = &dyn;
	gpi.layout = f.pipe_layout;
	gpi.renderPass = rp;
	gpi.subpass = 0;
	VkResult pr =
	    vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &f.pipeline);
	vkDestroyShaderModule(device, vert, nullptr);
	vkDestroyShaderModule(device, frag, nullptr);
	if (pr != VK_SUCCESS) {
		HFLOGE("hud_font: pipeline create failed (%d)", (int)pr);
		return false;
	}

	// ── 5. Mapped quad vertex buffer. ──
	if (!make_buffer(phys, device, sizeof(TextVtx) * kMaxVerts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                 f.vbuf, f.vbuf_mem)) {
		HFLOGE("hud_font: vertex buffer failed");
		return false;
	}
	vkMapMemory(device, f.vbuf_mem, 0, sizeof(TextVtx) * kMaxVerts, 0, &f.vbuf_mapped);

	f.ready = true;
	HFLOGI("hud_font: ready (atlas %ux%u, %.0fpx, %d baked rows)", f.atlas_w, f.atlas_h,
	       pixel_height, baked_rows);
	return true;
}

uint32_t
hud_font_build(HudFont &f, const char *text, float ox, float oy, float ndc_per_px, float line_step)
{
	if (!f.ready || !f.vbuf_mapped || !text) {
		return 0;
	}
	TextVtx *v = (TextVtx *)f.vbuf_mapped;
	uint32_t n = 0;
	const stbtt_bakedchar *cd = (const stbtt_bakedchar *)f.chars;

	int line = 0;
	float xpos = 0.0f;
	// Baseline one em below the line origin so glyphs hang DOWN from (ox, oy).
	float ypos = f.pixel_height;
	for (const char *s = text; *s; ++s) {
		if (*s == '\n') {
			++line;
			xpos = 0.0f;
			ypos = f.pixel_height;
			continue;
		}
		int c = (unsigned char)*s;
		if (c < HUD_FONT_FIRST_CHAR || c >= HUD_FONT_FIRST_CHAR + HUD_FONT_NUM_CHARS) {
			c = '?';
		}
		stbtt_aligned_quad q;
		stbtt_GetBakedQuad(cd, (int)f.atlas_w, (int)f.atlas_h, c - HUD_FONT_FIRST_CHAR, &xpos, &ypos,
		                   &q, 1);
		if (n + 6 > kMaxVerts) {
			break;
		}
		const float loy = oy + (float)line * line_step;
		// Pixel coords (q.x*/y*) → HUD NDC. q.x0/y0 are the glyph's top-left in
		// the pen's pixel space (origin at xpos=0, baseline at ypos).
		const float x0 = ox + q.x0 * ndc_per_px, x1 = ox + q.x1 * ndc_per_px;
		const float y0 = loy + q.y0 * ndc_per_px, y1 = loy + q.y1 * ndc_per_px;
		v[n++] = {{x0, y0}, {q.s0, q.t0}};
		v[n++] = {{x1, y0}, {q.s1, q.t0}};
		v[n++] = {{x1, y1}, {q.s1, q.t1}};
		v[n++] = {{x0, y0}, {q.s0, q.t0}};
		v[n++] = {{x1, y1}, {q.s1, q.t1}};
		v[n++] = {{x0, y1}, {q.s0, q.t1}};
	}
	return n;
}

void
hud_font_draw(HudFont &f, VkCommandBuffer cmd, const float mvp[16], const float color[4],
              uint32_t vert_count)
{
	if (!f.ready || vert_count == 0) {
		return;
	}
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, f.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, f.pipe_layout, 0, 1, &f.dset, 0,
	                        nullptr);
	float push[20];
	memcpy(push, mvp, sizeof(float) * 16);
	memcpy(push + 16, color, sizeof(float) * 4);
	vkCmdPushConstants(cmd, f.pipe_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
	                   0, sizeof(push), push);
	VkDeviceSize off = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &f.vbuf, &off);
	vkCmdDraw(cmd, vert_count, 1, 0, 0);
}

void
hud_font_destroy(HudFont &f)
{
	if (f.device == VK_NULL_HANDLE) {
		return;
	}
	VkDevice d = f.device;
	if (f.vbuf_mapped) {
		vkUnmapMemory(d, f.vbuf_mem);
		f.vbuf_mapped = nullptr;
	}
	if (f.vbuf) vkDestroyBuffer(d, f.vbuf, nullptr);
	if (f.vbuf_mem) vkFreeMemory(d, f.vbuf_mem, nullptr);
	if (f.pipeline) vkDestroyPipeline(d, f.pipeline, nullptr);
	if (f.pipe_layout) vkDestroyPipelineLayout(d, f.pipe_layout, nullptr);
	if (f.dpool) vkDestroyDescriptorPool(d, f.dpool, nullptr);
	if (f.dset_layout) vkDestroyDescriptorSetLayout(d, f.dset_layout, nullptr);
	if (f.sampler) vkDestroySampler(d, f.sampler, nullptr);
	if (f.atlas_view) vkDestroyImageView(d, f.atlas_view, nullptr);
	if (f.atlas_img) vkDestroyImage(d, f.atlas_img, nullptr);
	if (f.atlas_mem) vkFreeMemory(d, f.atlas_mem, nullptr);
	f = HudFont{};
}
