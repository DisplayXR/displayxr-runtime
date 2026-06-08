// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Masked 2D-over-3D composite implementation (#439 Phase 3).
 * @author David Fattal
 * @ingroup aux_vk
 */

#include "vk_local2d_composite.h"

#include "util/u_logging.h"

#include <string.h>

// Pre-compiled SPIR-V (GLSL source in shaders_local2d/, regen cmd in its README).
#include "shaders_local2d/fst_vert.h"
#include "shaders_local2d/masked_composite_frag.h"
#include "shaders_local2d/local2d_flatten_frag.h"

// Must match the masked_composite.frag push_constant block.
struct composite_push
{
	float dst_dims[2];
	float canvas_origin[2];
	float canvas_size[2];
	uint32_t use_rect_mask;
	uint32_t alpha_over; // #491: implicit path = premul "over" (twod + (1-a)*weave)
};

// Must match the local2d_flatten.frag push_constant block.
struct flatten_push
{
	float src_rect[4]; // xy = src origin, zw = src size (zw.y < 0 = flip_y)
};


/*
 *
 * Creation helpers.
 *
 */

static VkResult
make_shader(struct vk_bundle *vk, const uint32_t *code, size_t size, VkShaderModule *out)
{
	VkShaderModuleCreateInfo ci = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = size,
	    .pCode = code,
	};
	return vk->vkCreateShaderModule(vk->device, &ci, NULL, out);
}

// A single-color-attachment render pass that LOADs (or CLEARs) and keeps the
// attachment in COLOR_ATTACHMENT_OPTIMAL on both ends — the caller owns the
// transitions (mirrors vk_hud_blend's no-layout idiom).
static VkResult
make_render_pass(struct vk_bundle *vk, VkFormat fmt, VkAttachmentLoadOp load_op, VkRenderPass *out)
{
	VkAttachmentDescription att = {
	    .format = fmt,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = load_op,
	    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	    .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub = {
	    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
	    .colorAttachmentCount = 1,
	    .pColorAttachments = &ref,
	};
	VkRenderPassCreateInfo ci = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &att,
	    .subpassCount = 1,
	    .pSubpasses = &sub,
	};
	return vk->vkCreateRenderPass(vk->device, &ci, NULL, out);
}

static VkResult
make_sampler(struct vk_bundle *vk, VkFilter filter, VkSampler *out)
{
	VkSamplerCreateInfo ci = {
	    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	    .magFilter = filter,
	    .minFilter = filter,
	    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	return vk->vkCreateSampler(vk->device, &ci, NULL, out);
}

static VkResult
make_dsl(struct vk_bundle *vk, uint32_t binding_count, VkDescriptorSetLayout *out)
{
	VkDescriptorSetLayoutBinding bindings[3];
	for (uint32_t i = 0; i < binding_count; i++) {
		bindings[i] = (VkDescriptorSetLayoutBinding){
		    .binding = i,
		    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		    .descriptorCount = 1,
		    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		};
	}
	VkDescriptorSetLayoutCreateInfo ci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = binding_count,
	    .pBindings = bindings,
	};
	return vk->vkCreateDescriptorSetLayout(vk->device, &ci, NULL, out);
}

// Fullscreen-triangle graphics pipeline with the given fragment module, blend
// attachment, and render pass. Dynamic viewport/scissor; no vertex input.
static VkResult
make_pipeline(struct vk_bundle *vk,
              VkShaderModule vert,
              VkShaderModule frag,
              VkPipelineLayout layout,
              VkRenderPass rp,
              const VkPipelineColorBlendAttachmentState *blend_att,
              VkPipeline *out)
{
	VkPipelineShaderStageCreateInfo stages[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_VERTEX_BIT,
	        .module = vert,
	        .pName = "main",
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	        .module = frag,
	        .pName = "main",
	    },
	};
	VkPipelineVertexInputStateCreateInfo vi = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
	VkPipelineViewportStateCreateInfo vps = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	    .viewportCount = 1,
	    .scissorCount = 1};
	VkPipelineRasterizationStateCreateInfo rs = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	    .polygonMode = VK_POLYGON_MODE_FILL,
	    .cullMode = VK_CULL_MODE_NONE,
	    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	    .lineWidth = 1.0f};
	VkPipelineMultisampleStateCreateInfo ms = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
	VkPipelineColorBlendStateCreateInfo cb = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = blend_att};
	VkPipelineDepthStencilStateCreateInfo ds = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dyn = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	    .dynamicStateCount = 2,
	    .pDynamicStates = dyn_states};
	VkGraphicsPipelineCreateInfo ci = {
	    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	    .stageCount = 2,
	    .pStages = stages,
	    .pVertexInputState = &vi,
	    .pInputAssemblyState = &ia,
	    .pViewportState = &vps,
	    .pRasterizationState = &rs,
	    .pMultisampleState = &ms,
	    .pDepthStencilState = &ds,
	    .pColorBlendState = &cb,
	    .pDynamicState = &dyn,
	    .layout = layout,
	    .renderPass = rp,
	    .subpass = 0,
	};
	return vk->vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &ci, NULL, out);
}


/*
 *
 * Public API.
 *
 */

bool
vk_local2d_composite_init(struct vk_local2d_composite *lc,
                          struct vk_bundle *vk,
                          VkFormat target_fmt,
                          VkFormat scratch_fmt)
{
	lc->target_fmt = target_fmt;
	lc->scratch_fmt = scratch_fmt;
	lc->mask_fmt = VK_FORMAT_R8_UNORM;

	VkResult ret;

	ret = make_shader(vk, fst_vert_spv, sizeof(fst_vert_spv), &lc->vert_mod);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[local2d] vert shader: %d", ret);
		return false;
	}
	ret = make_shader(vk, masked_composite_frag_spv, sizeof(masked_composite_frag_spv),
	                  &lc->composite_frag_mod);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[local2d] composite frag shader: %d", ret);
		return false;
	}
	ret = make_shader(vk, local2d_flatten_frag_spv, sizeof(local2d_flatten_frag_spv),
	                  &lc->flatten_frag_mod);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[local2d] flatten frag shader: %d", ret);
		return false;
	}

	// Composite samples region-sized inputs 1:1 → point (byte-exact, no edge
	// bleed); flatten samples a sub-rect of an arbitrarily-scaled layer →
	// linear.
	if (make_sampler(vk, VK_FILTER_NEAREST, &lc->sampler_point) != VK_SUCCESS ||
	    make_sampler(vk, VK_FILTER_LINEAR, &lc->sampler_linear) != VK_SUCCESS) {
		U_LOG_E("[local2d] sampler creation failed");
		return false;
	}

	// Render passes.
	if (make_render_pass(vk, target_fmt, VK_ATTACHMENT_LOAD_OP_LOAD, &lc->composite_rp) != VK_SUCCESS ||
	    make_render_pass(vk, scratch_fmt, VK_ATTACHMENT_LOAD_OP_LOAD, &lc->flatten_rp) != VK_SUCCESS ||
	    make_render_pass(vk, lc->mask_fmt, VK_ATTACHMENT_LOAD_OP_CLEAR, &lc->mask_rp) != VK_SUCCESS) {
		U_LOG_E("[local2d] render pass creation failed");
		return false;
	}

	// Descriptor set layouts + pipeline layouts (with push constants).
	if (make_dsl(vk, 3, &lc->composite_dsl) != VK_SUCCESS ||
	    make_dsl(vk, 1, &lc->flatten_dsl) != VK_SUCCESS) {
		U_LOG_E("[local2d] descriptor set layout failed");
		return false;
	}

	VkPushConstantRange composite_pcr = {
	    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	    .offset = 0,
	    .size = sizeof(struct composite_push),
	};
	VkPipelineLayoutCreateInfo composite_pl = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount = 1,
	    .pSetLayouts = &lc->composite_dsl,
	    .pushConstantRangeCount = 1,
	    .pPushConstantRanges = &composite_pcr,
	};
	if (vk->vkCreatePipelineLayout(vk->device, &composite_pl, NULL, &lc->composite_layout) != VK_SUCCESS) {
		U_LOG_E("[local2d] composite pipeline layout failed");
		return false;
	}

	VkPushConstantRange flatten_pcr = {
	    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	    .offset = 0,
	    .size = sizeof(struct flatten_push),
	};
	VkPipelineLayoutCreateInfo flatten_pl = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount = 1,
	    .pSetLayouts = &lc->flatten_dsl,
	    .pushConstantRangeCount = 1,
	    .pPushConstantRanges = &flatten_pcr,
	};
	if (vk->vkCreatePipelineLayout(vk->device, &flatten_pl, NULL, &lc->flatten_layout) != VK_SUCCESS) {
		U_LOG_E("[local2d] flatten pipeline layout failed");
		return false;
	}

	// Composite: opaque (blend OFF), writes RGBA — the lerp REPLACES the pixel
	// (incl. alpha, so M=0 + transparent twod → final.a=0 → desktop shows
	// through). Matches D3D11 blend_opaque + write-mask ALL.
	VkPipelineColorBlendAttachmentState composite_blend = {
	    .blendEnable = VK_FALSE,
	    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	if (make_pipeline(vk, lc->vert_mod, lc->composite_frag_mod, lc->composite_layout, lc->composite_rp,
	                  &composite_blend, &lc->composite_pipe) != VK_SUCCESS) {
		U_LOG_E("[local2d] composite pipeline failed");
		return false;
	}

	// Flatten premultiplied-over: One / OneMinusSrcAlpha (color), One /
	// OneMinusSrcAlpha (alpha — preserve dst.a Porter-Duff "over").
	VkPipelineColorBlendAttachmentState premul_blend = {
	    .blendEnable = VK_TRUE,
	    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	    .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
	    .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	    .colorBlendOp = VK_BLEND_OP_ADD,
	    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
	    .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	    .alphaBlendOp = VK_BLEND_OP_ADD,
	};
	if (make_pipeline(vk, lc->vert_mod, lc->flatten_frag_mod, lc->flatten_layout, lc->flatten_rp,
	                  &premul_blend, &lc->flatten_premul_pipe) != VK_SUCCESS) {
		U_LOG_E("[local2d] flatten premul pipeline failed");
		return false;
	}

	// Flatten straight-alpha-over: SrcAlpha / OneMinusSrcAlpha.
	VkPipelineColorBlendAttachmentState unpremul_blend = premul_blend;
	unpremul_blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	if (make_pipeline(vk, lc->vert_mod, lc->flatten_frag_mod, lc->flatten_layout, lc->flatten_rp,
	                  &unpremul_blend, &lc->flatten_unpremul_pipe) != VK_SUCCESS) {
		U_LOG_E("[local2d] flatten unpremul pipeline failed");
		return false;
	}

	// One pool, reset each frame. 1 composite set (3 samplers) + up to
	// VK_LOCAL2D_MAX_SETS-1 flatten sets (1 sampler) per frame.
	VkDescriptorPoolSize pool_size = {
	    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    VK_LOCAL2D_MAX_SETS + 2, // +2 for the composite set's extra 2 samplers
	};
	VkDescriptorPoolCreateInfo dp_ci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	    .maxSets = VK_LOCAL2D_MAX_SETS,
	    .poolSizeCount = 1,
	    .pPoolSizes = &pool_size,
	};
	if (vk->vkCreateDescriptorPool(vk->device, &dp_ci, NULL, &lc->desc_pool) != VK_SUCCESS) {
		U_LOG_E("[local2d] descriptor pool failed");
		return false;
	}

	lc->initialized = true;
	return true;
}

void
vk_local2d_composite_begin_frame(struct vk_local2d_composite *lc, struct vk_bundle *vk)
{
	if (!lc->initialized) {
		return;
	}
	vk->vkResetDescriptorPool(vk->device, lc->desc_pool, 0);
}

// Allocate a descriptor set and bind @p count image views (all with @p sampler)
// to bindings 0..count-1. Returns VK_NULL_HANDLE on failure.
static VkDescriptorSet
alloc_image_set(struct vk_local2d_composite *lc,
                struct vk_bundle *vk,
                VkDescriptorSetLayout dsl,
                VkSampler sampler,
                const VkImageView *views,
                uint32_t count)
{
	VkDescriptorSetAllocateInfo ai = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	    .descriptorPool = lc->desc_pool,
	    .descriptorSetCount = 1,
	    .pSetLayouts = &dsl,
	};
	VkDescriptorSet set = VK_NULL_HANDLE;
	if (vk->vkAllocateDescriptorSets(vk->device, &ai, &set) != VK_SUCCESS) {
		U_LOG_E("[local2d] descriptor set alloc failed (pool exhausted?)");
		return VK_NULL_HANDLE;
	}

	VkDescriptorImageInfo infos[3];
	VkWriteDescriptorSet writes[3];
	for (uint32_t i = 0; i < count; i++) {
		infos[i] = (VkDescriptorImageInfo){
		    .sampler = sampler,
		    .imageView = views[i],
		    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		writes[i] = (VkWriteDescriptorSet){
		    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		    .dstSet = set,
		    .dstBinding = i,
		    .descriptorCount = 1,
		    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		    .pImageInfo = &infos[i],
		};
	}
	vk->vkUpdateDescriptorSets(vk->device, count, writes, 0, NULL);
	return set;
}

void
vk_local2d_composite_raster_mask(struct vk_local2d_composite *lc,
                                 struct vk_bundle *vk,
                                 VkCommandBuffer cmd,
                                 VkFramebuffer mask_fb,
                                 uint32_t w,
                                 uint32_t h,
                                 float base_value,
                                 const struct xrt_rect *rects,
                                 uint32_t rect_count,
                                 float rect_value)
{
	if (!lc->initialized || mask_fb == VK_NULL_HANDLE || w == 0 || h == 0) {
		return;
	}

	// LOAD_OP_CLEAR fills the whole attachment with base_value (R channel).
	VkClearValue clear = {.color = {.float32 = {base_value, 0.0f, 0.0f, 0.0f}}};
	VkRenderPassBeginInfo rp_bi = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = lc->mask_rp,
	    .framebuffer = mask_fb,
	    .renderArea = {{0, 0}, {w, h}},
	    .clearValueCount = 1,
	    .pClearValues = &clear,
	};
	vk->vkCmdBeginRenderPass(cmd, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

	// Per-rect clear inside the pass — the VK analog of ID3D11DeviceContext1::
	// ClearView with rects. All rects share rect_value, so one call.
	if (rects != NULL && rect_count > 0) {
		VkClearAttachment ca = {
		    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		    .colorAttachment = 0,
		    .clearValue = {.color = {.float32 = {rect_value, 0.0f, 0.0f, 0.0f}}},
		};
		VkClearRect cr[XRT_MAX_LAYERS];
		uint32_t n = 0;
		for (uint32_t i = 0; i < rect_count && n < XRT_MAX_LAYERS; i++) {
			int32_t left = rects[i].offset.w;
			int32_t top = rects[i].offset.h;
			int32_t right = left + rects[i].extent.w;
			int32_t bottom = top + rects[i].extent.h;
			if (left < 0) {
				left = 0;
			}
			if (top < 0) {
				top = 0;
			}
			if (right > (int32_t)w) {
				right = (int32_t)w;
			}
			if (bottom > (int32_t)h) {
				bottom = (int32_t)h;
			}
			if (right <= left || bottom <= top) {
				continue;
			}
			cr[n].rect.offset.x = left;
			cr[n].rect.offset.y = top;
			cr[n].rect.extent.width = (uint32_t)(right - left);
			cr[n].rect.extent.height = (uint32_t)(bottom - top);
			cr[n].baseArrayLayer = 0;
			cr[n].layerCount = 1;
			n++;
		}
		if (n > 0) {
			vk->vkCmdClearAttachments(cmd, 1, &ca, n, cr);
		}
	}

	vk->vkCmdEndRenderPass(cmd);
}

void
vk_local2d_composite_flatten_draw(struct vk_local2d_composite *lc,
                                  struct vk_bundle *vk,
                                  VkCommandBuffer cmd,
                                  VkFramebuffer scratch_fb,
                                  uint32_t fb_w,
                                  uint32_t fb_h,
                                  VkImageView src_view,
                                  int32_t dst_x,
                                  int32_t dst_y,
                                  uint32_t dst_w,
                                  uint32_t dst_h,
                                  float src_x,
                                  float src_y,
                                  float src_w,
                                  float src_h,
                                  bool unpremultiplied)
{
	if (!lc->initialized || scratch_fb == VK_NULL_HANDLE || src_view == VK_NULL_HANDLE || dst_w == 0 ||
	    dst_h == 0) {
		return;
	}

	VkDescriptorSet set = alloc_image_set(lc, vk, lc->flatten_dsl, lc->sampler_linear, &src_view, 1);
	if (set == VK_NULL_HANDLE) {
		return;
	}

	VkRenderPassBeginInfo rp_bi = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = lc->flatten_rp,
	    .framebuffer = scratch_fb,
	    .renderArea = {{0, 0}, {fb_w, fb_h}},
	};
	vk->vkCmdBeginRenderPass(cmd, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

	// Viewport restricts output to the clipped dest sub-rect; uv [0,1] over it
	// maps through src_rect into the source layer.
	VkViewport vp = {(float)dst_x, (float)dst_y, (float)dst_w, (float)dst_h, 0.0f, 1.0f};
	vk->vkCmdSetViewport(cmd, 0, 1, &vp);
	VkRect2D scissor = {{dst_x, dst_y}, {dst_w, dst_h}};
	vk->vkCmdSetScissor(cmd, 0, 1, &scissor);

	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                      unpremultiplied ? lc->flatten_unpremul_pipe : lc->flatten_premul_pipe);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lc->flatten_layout, 0, 1, &set, 0,
	                            NULL);

	struct flatten_push pc = {.src_rect = {src_x, src_y, src_w, src_h}};
	vk->vkCmdPushConstants(cmd, lc->flatten_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

	vk->vkCmdDraw(cmd, 3, 1, 0, 0);
	vk->vkCmdEndRenderPass(cmd);
}

void
vk_local2d_composite_draw(struct vk_local2d_composite *lc,
                          struct vk_bundle *vk,
                          VkCommandBuffer cmd,
                          VkFramebuffer target_fb,
                          uint32_t fb_w,
                          uint32_t fb_h,
                          VkImageView twod_view,
                          VkImageView mask_view,
                          VkImageView weave_view,
                          uint32_t region_w,
                          uint32_t region_h,
                          int32_t cx,
                          int32_t cy,
                          uint32_t cw,
                          uint32_t ch,
                          bool alpha_over)
{
	if (!lc->initialized || target_fb == VK_NULL_HANDLE || twod_view == VK_NULL_HANDLE || region_w == 0 ||
	    region_h == 0) {
		return;
	}
	// The mask-lerp path samples mask + weave; both must be present together.
	const bool use_rect_mask = (mask_view == VK_NULL_HANDLE);
	if (!use_rect_mask && weave_view == VK_NULL_HANDLE) {
		return;
	}

	// Bindings 0=twod, 1=mask, 2=weave. On the rect path the shader never
	// samples mask/weave, but the descriptor set must still be complete — bind
	// twod as a harmless placeholder for the unused slots.
	VkImageView views[3] = {
	    twod_view,
	    use_rect_mask ? twod_view : mask_view,
	    use_rect_mask ? twod_view : weave_view,
	};
	VkDescriptorSet set = alloc_image_set(lc, vk, lc->composite_dsl, lc->sampler_point, views, 3);
	if (set == VK_NULL_HANDLE) {
		return;
	}

	VkRenderPassBeginInfo rp_bi = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = lc->composite_rp,
	    .framebuffer = target_fb,
	    .renderArea = {{0, 0}, {fb_w, fb_h}},
	};
	vk->vkCmdBeginRenderPass(cmd, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

	// Region-sized output at the top-left anchor (#464). uv [0,1] spans the
	// region; region-sized inputs sample 1:1.
	VkViewport vp = {0.0f, 0.0f, (float)region_w, (float)region_h, 0.0f, 1.0f};
	vk->vkCmdSetViewport(cmd, 0, 1, &vp);
	VkRect2D scissor = {{0, 0}, {region_w, region_h}};
	vk->vkCmdSetScissor(cmd, 0, 1, &scissor);

	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lc->composite_pipe);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lc->composite_layout, 0, 1, &set, 0,
	                            NULL);

	struct composite_push pc = {
	    .dst_dims = {(float)region_w, (float)region_h},
	    .canvas_origin = {(float)cx, (float)cy},
	    .canvas_size = {(float)cw, (float)ch},
	    .use_rect_mask = use_rect_mask ? 1u : 0u,
	    .alpha_over = (!use_rect_mask && alpha_over) ? 1u : 0u,
	};
	vk->vkCmdPushConstants(cmd, lc->composite_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

	vk->vkCmdDraw(cmd, 3, 1, 0, 0);
	vk->vkCmdEndRenderPass(cmd);
}

void
vk_local2d_composite_fini(struct vk_local2d_composite *lc, struct vk_bundle *vk)
{
	if (!lc->initialized) {
		return;
	}

#define DESTROY(fn, h)                                                                                              \
	do {                                                                                                       \
		if ((h) != VK_NULL_HANDLE) {                                                                        \
			vk->fn(vk->device, (h), NULL);                                                              \
			(h) = VK_NULL_HANDLE;                                                                       \
		}                                                                                                  \
	} while (0)

	DESTROY(vkDestroyPipeline, lc->composite_pipe);
	DESTROY(vkDestroyPipeline, lc->flatten_premul_pipe);
	DESTROY(vkDestroyPipeline, lc->flatten_unpremul_pipe);
	DESTROY(vkDestroyPipelineLayout, lc->composite_layout);
	DESTROY(vkDestroyPipelineLayout, lc->flatten_layout);
	DESTROY(vkDestroyDescriptorSetLayout, lc->composite_dsl);
	DESTROY(vkDestroyDescriptorSetLayout, lc->flatten_dsl);
	DESTROY(vkDestroyDescriptorPool, lc->desc_pool);
	DESTROY(vkDestroyRenderPass, lc->composite_rp);
	DESTROY(vkDestroyRenderPass, lc->flatten_rp);
	DESTROY(vkDestroyRenderPass, lc->mask_rp);
	DESTROY(vkDestroySampler, lc->sampler_point);
	DESTROY(vkDestroySampler, lc->sampler_linear);
	DESTROY(vkDestroyShaderModule, lc->vert_mod);
	DESTROY(vkDestroyShaderModule, lc->composite_frag_mod);
	DESTROY(vkDestroyShaderModule, lc->flatten_frag_mod);

#undef DESTROY

	memset(lc, 0, sizeof(*lc));
}
