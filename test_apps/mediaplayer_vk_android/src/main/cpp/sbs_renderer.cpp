// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

#include "sbs_renderer.h"

#include <android/log.h>
#include <cstring>
#include <vector>

#include "fullscreen.vert.h"  // fullscreen_vert_data (SPIR-V)
#include "sbs.frag.h"         // sbs_frag_data (SPIR-V)

#define LOG_TAG "mediaplayer_vk_android"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// Must match shaders/sbs.frag's push_constant block (24 bytes).
struct SbsPush {
	float uvOffset[2];
	float uvScale[2];
	int32_t mode;     // 0 = RGBA (image MVP)
	float fullRange;  // 1 = full/JPEG range
};

uint32_t
SbsRenderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
		if ((typeBits & (1u << i)) &&
		    (mp.memoryTypes[i].propertyFlags & props) == props) {
			return i;
		}
	}
	return UINT32_MAX;
}

bool
SbsRenderer::init(VkPhysicalDevice phys, VkDevice device, VkQueue queue,
                  uint32_t queueFamily, VkFormat format)
{
	phys_ = phys;
	device_ = device;
	queue_ = queue;
	queueFamily_ = queueFamily;
	format_ = format;

	// ── Color-only render pass. The swapchain image arrives undefined (we
	// CLEAR), and the runtime expects COLOR_ATTACHMENT_OPTIMAL at release. ──
	VkAttachmentDescription att = {};
	att.format = format_;
	att.samples = VK_SAMPLE_COUNT_1_BIT;
	att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub = {};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &ref;
	VkRenderPassCreateInfo rpci = {};
	rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpci.attachmentCount = 1;
	rpci.pAttachments = &att;
	rpci.subpassCount = 1;
	rpci.pSubpasses = &sub;
	if (vkCreateRenderPass(device_, &rpci, nullptr, &renderPass_) != VK_SUCCESS) {
		LOGE("vkCreateRenderPass failed");
		return false;
	}

	// ── Descriptor set layout: 3 combined image samplers (plane 0..2). ──
	VkDescriptorSetLayoutBinding b[3] = {};
	for (uint32_t i = 0; i < 3; ++i) {
		b[i].binding = i;
		b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo dslci = {};
	dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dslci.bindingCount = 3;
	dslci.pBindings = b;
	if (vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &setLayout_) != VK_SUCCESS) {
		LOGE("vkCreateDescriptorSetLayout failed");
		return false;
	}

	VkPushConstantRange pc = {};
	pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	pc.offset = 0;
	pc.size = sizeof(SbsPush);
	VkPipelineLayoutCreateInfo plci = {};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &setLayout_;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pc;
	if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipeLayout_) != VK_SUCCESS) {
		LOGE("vkCreatePipelineLayout failed");
		return false;
	}

	// ── Pipeline (fullscreen triangle, no vertex input). ──
	auto makeModule = [&](const uint32_t *code, size_t bytes) {
		VkShaderModuleCreateInfo smci = {};
		smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		smci.codeSize = bytes;
		smci.pCode = code;
		VkShaderModule m = VK_NULL_HANDLE;
		vkCreateShaderModule(device_, &smci, nullptr, &m);
		return m;
	};
	VkShaderModule vert = makeModule(fullscreen_vert_data, sizeof(fullscreen_vert_data));
	VkShaderModule frag = makeModule(sbs_frag_data, sizeof(sbs_frag_data));
	if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
		LOGE("shader module creation failed");
		return false;
	}
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
	VkPipelineColorBlendStateCreateInfo cb = {};
	cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;
	VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dyn = {};
	dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dyn.dynamicStateCount = 2;
	dyn.pDynamicStates = dynStates;
	VkGraphicsPipelineCreateInfo gp = {};
	gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	gp.stageCount = 2;
	gp.pStages = stages;
	gp.pVertexInputState = &vi;
	gp.pInputAssemblyState = &ia;
	gp.pViewportState = &vp;
	gp.pRasterizationState = &rs;
	gp.pMultisampleState = &ms;
	gp.pColorBlendState = &cb;
	gp.pDynamicState = &dyn;
	gp.layout = pipeLayout_;
	gp.renderPass = renderPass_;
	VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_);
	vkDestroyShaderModule(device_, vert, nullptr);
	vkDestroyShaderModule(device_, frag, nullptr);
	if (r != VK_SUCCESS) {
		LOGE("vkCreateGraphicsPipelines failed: %d", (int)r);
		return false;
	}

	// Sampler: linear, clamp-to-edge (no wrap bleed across the SBS seam at 0.5).
	VkSamplerCreateInfo sci = {};
	sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (vkCreateSampler(device_, &sci, nullptr, &sampler_) != VK_SUCCESS) {
		LOGE("vkCreateSampler failed");
		return false;
	}

	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
	VkDescriptorPoolCreateInfo dpci = {};
	dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpci.maxSets = 1;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &ps;
	if (vkCreateDescriptorPool(device_, &dpci, nullptr, &descPool_) != VK_SUCCESS) {
		LOGE("vkCreateDescriptorPool failed");
		return false;
	}
	VkDescriptorSetAllocateInfo dsai = {};
	dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool = descPool_;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &setLayout_;
	if (vkAllocateDescriptorSets(device_, &dsai, &descSet_) != VK_SUCCESS) {
		LOGE("vkAllocateDescriptorSets failed");
		return false;
	}

	VkCommandPoolCreateInfo cpci = {};
	cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpci.queueFamilyIndex = queueFamily_;
	if (vkCreateCommandPool(device_, &cpci, nullptr, &cmdPool_) != VK_SUCCESS) {
		LOGE("vkCreateCommandPool failed");
		return false;
	}
	VkCommandBufferAllocateInfo cbai = {};
	cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandPool = cmdPool_;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(device_, &cbai, &cmd_) != VK_SUCCESS) {
		LOGE("vkAllocateCommandBuffers failed");
		return false;
	}
	VkFenceCreateInfo fci = {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (vkCreateFence(device_, &fci, nullptr, &fence_) != VK_SUCCESS) {
		LOGE("vkCreateFence failed");
		return false;
	}
	LOGI("SbsRenderer initialized (format=0x%x)", (uint32_t)format_);
	return true;
}

void
SbsRenderer::destroyTexture()
{
	if (texView_ != VK_NULL_HANDLE) vkDestroyImageView(device_, texView_, nullptr);
	if (texImage_ != VK_NULL_HANDLE) vkDestroyImage(device_, texImage_, nullptr);
	if (texMem_ != VK_NULL_HANDLE) vkFreeMemory(device_, texMem_, nullptr);
	texView_ = VK_NULL_HANDLE;
	texImage_ = VK_NULL_HANDLE;
	texMem_ = VK_NULL_HANDLE;
}

void
SbsRenderer::bindDescriptor()
{
	VkDescriptorImageInfo info = {};
	info.sampler = sampler_;
	info.imageView = texView_;
	info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkWriteDescriptorSet w[3] = {};
	for (uint32_t i = 0; i < 3; ++i) {  // mode 0 uses slot 0; bind all to keep valid
		w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[i].dstSet = descSet_;
		w[i].dstBinding = i;
		w[i].descriptorCount = 1;
		w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w[i].pImageInfo = &info;
	}
	vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
}

bool
SbsRenderer::uploadTexture(const uint8_t *rgba, uint32_t width, uint32_t height)
{
	vkDeviceWaitIdle(device_);
	destroyTexture();

	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent = {width, height, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(device_, &ici, nullptr, &texImage_) != VK_SUCCESS) return false;
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(device_, texImage_, &mr);
	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (vkAllocateMemory(device_, &mai, nullptr, &texMem_) != VK_SUCCESS) return false;
	vkBindImageMemory(device_, texImage_, texMem_, 0);

	// Staging buffer (host-visible) + copy.
	const VkDeviceSize sz = (VkDeviceSize)width * height * 4;
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory stagingMem = VK_NULL_HANDLE;
	VkBufferCreateInfo bci = {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = sz;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	vkCreateBuffer(device_, &bci, nullptr, &staging);
	VkMemoryRequirements bmr;
	vkGetBufferMemoryRequirements(device_, staging, &bmr);
	VkMemoryAllocateInfo bmai = {};
	bmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	bmai.allocationSize = bmr.size;
	bmai.memoryTypeIndex = findMemoryType(
	    bmr.memoryTypeBits,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	vkAllocateMemory(device_, &bmai, nullptr, &stagingMem);
	vkBindBufferMemory(device_, staging, stagingMem, 0);
	void *mapped = nullptr;
	vkMapMemory(device_, stagingMem, 0, sz, 0, &mapped);
	std::memcpy(mapped, rgba, (size_t)sz);
	vkUnmapMemory(device_, stagingMem);

	// Record: UNDEFINED->TRANSFER_DST, copy, TRANSFER_DST->SHADER_READ_ONLY.
	vkResetCommandBuffer(cmd_, 0);
	VkCommandBufferBeginInfo cbbi = {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd_, &cbbi);
	VkImageMemoryBarrier toDst = {};
	toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toDst.image = texImage_;
	toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
	VkBufferImageCopy region = {};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {width, height, 1};
	vkCmdCopyBufferToImage(cmd_, staging, texImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
	                       &region);
	VkImageMemoryBarrier toRead = toDst;
	toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
	                     &toRead);
	vkEndCommandBuffer(cmd_);
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd_;
	vkResetFences(device_, 1, &fence_);
	vkQueueSubmit(queue_, 1, &si, fence_);
	vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
	vkDestroyBuffer(device_, staging, nullptr);
	vkFreeMemory(device_, stagingMem, nullptr);

	VkImageViewCreateInfo vci = {};
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = texImage_;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = VK_FORMAT_R8G8B8A8_UNORM;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (vkCreateImageView(device_, &vci, nullptr, &texView_) != VK_SUCCESS) return false;
	bindDescriptor();
	LOGI("uploadTexture %ux%u", width, height);
	return true;
}

const SbsRenderer::Target &
SbsRenderer::targetFor(VkImage image, uint32_t w, uint32_t h)
{
	auto it = targets_.find(image);
	if (it != targets_.end() && it->second.w == w && it->second.h == h) {
		return it->second;
	}
	Target t;
	t.w = w;
	t.h = h;
	VkImageViewCreateInfo vci = {};
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = format_;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCreateImageView(device_, &vci, nullptr, &t.view);
	VkFramebufferCreateInfo fci = {};
	fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fci.renderPass = renderPass_;
	fci.attachmentCount = 1;
	fci.pAttachments = &t.view;
	fci.width = w;
	fci.height = h;
	fci.layers = 1;
	vkCreateFramebuffer(device_, &fci, nullptr, &t.fb);
	targets_[image] = t;
	return targets_[image];
}

void
SbsRenderer::drawEye(VkImage image, uint32_t w, uint32_t h, float offX, float offY,
                     float scaleX, float scaleY)
{
	if (texView_ == VK_NULL_HANDLE) return;
	const Target &t = targetFor(image, w, h);

	vkResetCommandBuffer(cmd_, 0);
	VkCommandBufferBeginInfo cbbi = {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd_, &cbbi);

	VkClearValue clear = {};
	clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
	VkRenderPassBeginInfo rpbi = {};
	rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rpbi.renderPass = renderPass_;
	rpbi.framebuffer = t.fb;
	rpbi.renderArea.extent = {w, h};
	rpbi.clearValueCount = 1;
	rpbi.pClearValues = &clear;
	vkCmdBeginRenderPass(cmd_, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport vp = {0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
	VkRect2D sc = {{0, 0}, {w, h}};
	vkCmdSetViewport(cmd_, 0, 1, &vp);
	vkCmdSetScissor(cmd_, 0, 1, &sc);
	vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
	vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout_, 0, 1, &descSet_,
	                        0, nullptr);
	SbsPush push = {};
	push.uvOffset[0] = offX;
	push.uvOffset[1] = offY;
	push.uvScale[0] = scaleX;
	push.uvScale[1] = scaleY;
	push.mode = 0;
	push.fullRange = 1.0f;
	vkCmdPushConstants(cmd_, pipeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	vkCmdDraw(cmd_, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd_);
	vkEndCommandBuffer(cmd_);

	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd_;
	vkResetFences(device_, 1, &fence_);
	vkQueueSubmit(queue_, 1, &si, fence_);
	vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
}

void
SbsRenderer::cleanup()
{
	if (device_ == VK_NULL_HANDLE) return;
	vkDeviceWaitIdle(device_);
	for (auto &kv : targets_) {
		if (kv.second.fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, kv.second.fb, nullptr);
		if (kv.second.view != VK_NULL_HANDLE) vkDestroyImageView(device_, kv.second.view, nullptr);
	}
	targets_.clear();
	destroyTexture();
	if (fence_) vkDestroyFence(device_, fence_, nullptr);
	if (cmdPool_) vkDestroyCommandPool(device_, cmdPool_, nullptr);
	if (descPool_) vkDestroyDescriptorPool(device_, descPool_, nullptr);
	if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
	if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
	if (pipeLayout_) vkDestroyPipelineLayout(device_, pipeLayout_, nullptr);
	if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
	if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
	fence_ = VK_NULL_HANDLE;
	cmdPool_ = VK_NULL_HANDLE;
	descPool_ = VK_NULL_HANDLE;
	sampler_ = VK_NULL_HANDLE;
	pipeline_ = VK_NULL_HANDLE;
	pipeLayout_ = VK_NULL_HANDLE;
	setLayout_ = VK_NULL_HANDLE;
	renderPass_ = VK_NULL_HANDLE;
	device_ = VK_NULL_HANDLE;
}
