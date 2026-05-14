// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia display processor: wraps SR SDK Vulkan weaver
 *         as an @ref xrt_display_processor.
 *
 * The display processor owns the leiasr handle — it creates it
 * via the factory function and destroys it on cleanup.
 *
 * The SR SDK weaver expects side-by-side (SBS) stereo input. The Leia
 * device defines its 3D mode as tile_columns=2, tile_rows=1, so the
 * compositor always delivers SBS. The compositor crop-blit guarantees
 * the atlas texture dimensions match exactly 2*view_width x view_height.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_processor.h"
#include "leia_sr.h"

#include "xrt/xrt_display_metrics.h"
#include "vk/vk_helpers.h"
#include "util/u_logging.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>

// SPIR-V chroma-key shaders (generated at build time by spirv_shaders()).
#include "leia/shaders/fullscreen_tri.vert.h"
#include "leia/shaders/ck_fill.frag.h"
#include "leia/shaders/ck_strip.frag.h"

#ifdef _WIN32
#include "leia_bg_capture_win.h"
#include "leia/shaders/compose_under_bg.frag.h"
#include "leia/shaders/alpha_gate.frag.h"
#endif


// Default chroma key when the app didn't supply one (set_chroma_key key=0).
// Magenta — matches the D3D11/D3D12 DPs' kDefaultChromaKey for cross-API parity.
// Layout 0x00BBGGRR: R=0xFF, G=0x00, B=0xFF.
static constexpr uint32_t kDefaultChromaKey = 0x00FF00FF;

// Maximum number of cached strip framebuffers (one per swapchain image view).
// Typical swapchains have 2–3 images; 8 is a safe upper bound.
static constexpr uint32_t kMaxStripFramebuffers = 8;


/*!
 * Implementation struct wrapping leiasr as xrt_display_processor.
 */
struct leia_display_processor
{
	struct xrt_display_processor base;
	struct leiasr *leiasr; //!< Owned — destroyed in leia_dp_destroy.
	struct vk_bundle *vk;  //!< Cached vk_bundle (not owned).

	VkRenderPass render_pass;   //!< Render pass for framebuffer compatibility.
	uint32_t view_count; //!< Active mode view count (1=2D, 2=stereo).

	//
	// Chroma-key transparency support (lazy-initialized on first frame).
	//
	// When @ref ck_enabled is true, process_atlas runs:
	//   1. Pre-weave fill: atlas RGBA -> ck_fill_image (alpha=0 -> chroma_rgb,
	//      output alpha=1) so the SR weaver receives opaque RGB.
	//   2. Pass ck_fill_view (image view) to the weaver instead of the
	//      caller-supplied atlas.
	//   3. Post-weave strip: copy back-buffer -> ck_strip_image, then run
	//      strip render pass back into the back-buffer (chroma match -> alpha=0,
	//      RGB premultiplied for DWM).
	//
	// All resources lazy-allocated; ck_init_pipeline is called once on the
	// first frame the strip pass needs to run.
	//
	bool ck_enabled;
	uint32_t ck_color;             //!< 0x00BBGGRR; effective key.

	// Pipeline / descriptor / sampler shared by both fill and strip.
	bool ck_inited;                //!< True once the pipeline objects are built.
	VkRenderPass ck_fill_rp;       //!< Render pass writing to ck_fill_image.
	VkRenderPass ck_strip_rp;      //!< Render pass writing to swapchain image (final layout PRESENT_SRC_KHR).
	VkPipelineLayout ck_pipeline_layout;
	VkDescriptorSetLayout ck_desc_layout;
	VkDescriptorPool ck_desc_pool;
	VkDescriptorSet ck_fill_set;
	VkDescriptorSet ck_strip_set;
	VkPipeline ck_fill_pipeline;
	VkPipeline ck_strip_pipeline;
	VkSampler ck_sampler;          //!< Point/clamp; required for strip's exact-equality test.
	VkFormat ck_strip_target_format; //!< Swapchain format the strip render pass was built for.

	// Pre-weave fill target — RT-bindable and SRV-readable.
	VkImage ck_fill_image;
	VkImageView ck_fill_view;
	VkDeviceMemory ck_fill_mem;
	VkFramebuffer ck_fill_fb;
	uint32_t ck_fill_w, ck_fill_h;

	// Post-weave strip source — copy of the back buffer for the strip pass to sample.
	VkImage ck_strip_image;
	VkImageView ck_strip_view;
	VkDeviceMemory ck_strip_mem;
	uint32_t ck_strip_w, ck_strip_h;

	// Per-swapchain-image strip-output cache. The DP vtable doesn't expose a
	// VkImageView for the target, only the target VkImage — so we own a view
	// per image and cache the framebuffer alongside it.
	struct {
		VkImage image;               //!< Cache key: target swapchain VkImage.
		VkImageView view;            //!< Owned by the DP.
		VkFramebuffer fb;            //!< Owned by the DP.
		uint32_t w, h;
	} ck_strip_fbs[kMaxStripFramebuffers];
	uint32_t ck_strip_fbs_count;

	//
	// Compose-under-bg transparency support (preferred over chroma-key on
	// Windows when WGC desktop capture is available). Reuses ck_fill_image
	// /ck_fill_view / ck_fill_fb / ck_fill_rp as the intermediate target —
	// same R8G8B8A8_UNORM format. Distinct pipeline (2 SRVs + 24-byte push
	// constants for bg_uv + tile_count) and descriptor set.
	//
	// On non-Windows or WGC-init failure, bg_compose_enabled stays false and
	// the chroma-key path runs unchanged.
	//
	void *hwnd_opaque;                       //!< HWND from factory (Win-only). void* so the struct stays cross-platform.
	struct leia_bg_capture *bg_capture;      //!< Owned (Win-only). NULL → fall back to ck.
	bool bg_compose_enabled;

	VkImage bg_image;
	VkImageView bg_view;
	VkDeviceMemory bg_mem;
	uint32_t bg_w, bg_h;

	VkSampler compose_sampler;               //!< Linear/clamp (vs ck's NEAREST).
	VkDescriptorSetLayout compose_desc_layout;
	VkPipelineLayout compose_pipeline_layout;
	VkDescriptorPool compose_desc_pool;
	VkDescriptorSet compose_set;
	VkPipeline compose_pipeline;
	bool compose_inited;

	// Post-weave alpha-gate (compose-mode replacement for ck_strip).
	// 2-binding descriptor set (back-buffer copy + atlas), 16-byte push
	// constants (tile_count). Uses ck_strip_rp as the output render pass
	// (writes the swap-chain image, finalLayout = PRESENT_SRC_KHR).
	VkDescriptorSetLayout alpha_gate_desc_layout;
	VkPipelineLayout alpha_gate_pipeline_layout;
	VkDescriptorPool alpha_gate_desc_pool;
	VkDescriptorSet alpha_gate_set;
	VkPipeline alpha_gate_pipeline;
};

static inline struct leia_display_processor *
leia_display_processor(struct xrt_display_processor *xdp)
{
	return (struct leia_display_processor *)xdp;
}


/*
 *
 * Chroma-key fill/strip helpers (transparency support).
 *
 * Lazy-allocated on first frame the pass runs. ck_should_run() gates the
 * whole flow — when false (the common case) none of these execute and
 * process_atlas behaves identically to the pre-transparency path.
 *
 * The fill pass writes ck_fill_image (R8G8B8A8_UNORM) and the SR weaver
 * samples it instead of the original atlas. The strip pass copies the
 * presented swapchain image to ck_strip_image then renders premultiplied
 * alpha back into the swapchain image (PRESENT_SRC_KHR -> ... ->
 * PRESENT_SRC_KHR via per-target-view framebuffers).
 *
 */

static bool
ck_should_run(struct leia_display_processor *ldp)
{
	return ldp->ck_enabled && ldp->ck_color != 0;
}

static VkResult
ck_create_shader_module(struct vk_bundle *vk,
                        const uint32_t *code,
                        size_t code_size,
                        VkShaderModule *out_module)
{
	VkShaderModuleCreateInfo ci = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = code_size,
	    .pCode = code,
	};
	return vk->vkCreateShaderModule(vk->device, &ci, NULL, out_module);
}

static void
ck_unpack_chroma_rgb(uint32_t color, float out_rgb[3])
{
	// 0x00BBGGRR layout matches D3D11/D3D12 DPs.
	uint8_t r = (uint8_t)((color >> 0) & 0xff);
	uint8_t g = (uint8_t)((color >> 8) & 0xff);
	uint8_t b = (uint8_t)((color >> 16) & 0xff);
	out_rgb[0] = (float)r / 255.0f;
	out_rgb[1] = (float)g / 255.0f;
	out_rgb[2] = (float)b / 255.0f;
}

// Build the descriptor set layout / pipeline layout / sampler / render passes /
// pipelines. Idempotent: returns true if already inited.
static bool
ck_init_pipeline(struct leia_display_processor *ldp, VkFormat target_format)
{
	if (ldp->ck_inited && ldp->ck_strip_target_format == target_format) {
		return true;
	}
	if (ldp->ck_inited) {
		// Target format changed (rare — swapchain re-created with different
		// format). Tear down strip pipeline only; rebuild below.
		// In the common case the format never changes; the cleanup happens
		// in ck_release_resources at destroy time.
		U_LOG_W("Leia VK DP: ck strip render pass format change %d -> %d not supported (no rebuild)",
		        (int)ldp->ck_strip_target_format, (int)target_format);
		return false;
	}

	struct vk_bundle *vk = ldp->vk;
	VkResult res;

	// Render pass for the fill pass: writes R8G8B8A8_UNORM, no presentation.
	{
		VkAttachmentDescription att = {
		    .format = VK_FORMAT_R8G8B8A8_UNORM,
		    .samples = VK_SAMPLE_COUNT_1_BIT,
		    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
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
		VkRenderPassCreateInfo rpi = {
		    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		    .attachmentCount = 1,
		    .pAttachments = &att,
		    .subpassCount = 1,
		    .pSubpasses = &sub,
		};
		res = vk->vkCreateRenderPass(vk->device, &rpi, NULL, &ldp->ck_fill_rp);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: ck_fill_rp create failed: %d", res);
			return false;
		}
	}

	// Render pass for the strip pass: writes the swapchain format and
	// transitions to PRESENT_SRC_KHR so the swapchain image is presentable
	// when we end the render pass.
	{
		VkAttachmentDescription att = {
		    .format = target_format,
		    .samples = VK_SAMPLE_COUNT_1_BIT,
		    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		    .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		};
		VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription sub = {
		    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		    .colorAttachmentCount = 1,
		    .pColorAttachments = &ref,
		};
		VkRenderPassCreateInfo rpi = {
		    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		    .attachmentCount = 1,
		    .pAttachments = &att,
		    .subpassCount = 1,
		    .pSubpasses = &sub,
		};
		res = vk->vkCreateRenderPass(vk->device, &rpi, NULL, &ldp->ck_strip_rp);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: ck_strip_rp create failed: %d", res);
			return false;
		}
		ldp->ck_strip_target_format = target_format;
	}

	// Descriptor set layout: 1 combined image sampler at binding 0.
	{
		VkDescriptorSetLayoutBinding b = {
		    .binding = 0,
		    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		    .descriptorCount = 1,
		    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		};
		VkDescriptorSetLayoutCreateInfo ci = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		    .bindingCount = 1,
		    .pBindings = &b,
		};
		res = vk->vkCreateDescriptorSetLayout(vk->device, &ci, NULL, &ldp->ck_desc_layout);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: ck desc set layout failed: %d", res);
			return false;
		}
	}

	// Pipeline layout: push constants for chroma_rgb, single descriptor set.
	{
		VkPushConstantRange pc = {
		    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		    .offset = 0,
		    .size = 4 * sizeof(float), // vec3 chroma_rgb + pad
		};
		VkPipelineLayoutCreateInfo pli = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		    .setLayoutCount = 1,
		    .pSetLayouts = &ldp->ck_desc_layout,
		    .pushConstantRangeCount = 1,
		    .pPushConstantRanges = &pc,
		};
		res = vk->vkCreatePipelineLayout(vk->device, &pli, NULL, &ldp->ck_pipeline_layout);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: ck pipeline layout failed: %d", res);
			return false;
		}
	}

	// Sampler: NEAREST/CLAMP — strip's exact-equality test must not see
	// linear blending across the chroma boundary.
	{
		VkSamplerCreateInfo si = {
		    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		    .magFilter = VK_FILTER_NEAREST,
		    .minFilter = VK_FILTER_NEAREST,
		    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		    .maxLod = 1.0f,
		};
		res = vk->vkCreateSampler(vk->device, &si, NULL, &ldp->ck_sampler);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: ck sampler failed: %d", res);
			return false;
		}
	}

	// Descriptor pool + 2 sets (fill, strip).
	{
		VkDescriptorPoolSize size = {
		    .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		    .descriptorCount = 2,
		};
		VkDescriptorPoolCreateInfo dpi = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		    .maxSets = 2,
		    .poolSizeCount = 1,
		    .pPoolSizes = &size,
		};
		res = vk->vkCreateDescriptorPool(vk->device, &dpi, NULL, &ldp->ck_desc_pool);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: ck desc pool failed: %d", res);
			return false;
		}

		VkDescriptorSetLayout layouts[2] = {ldp->ck_desc_layout, ldp->ck_desc_layout};
		VkDescriptorSetAllocateInfo ai = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		    .descriptorPool = ldp->ck_desc_pool,
		    .descriptorSetCount = 2,
		    .pSetLayouts = layouts,
		};
		VkDescriptorSet sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
		res = vk->vkAllocateDescriptorSets(vk->device, &ai, sets);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: ck desc set alloc failed: %d", res);
			return false;
		}
		ldp->ck_fill_set = sets[0];
		ldp->ck_strip_set = sets[1];
	}

	// Compile shaders + build the two graphics pipelines.
	VkShaderModule vs = VK_NULL_HANDLE;
	VkShaderModule fs_fill = VK_NULL_HANDLE;
	VkShaderModule fs_strip = VK_NULL_HANDLE;
	res = ck_create_shader_module(vk, leia_shaders_fullscreen_tri_vert,
	                              sizeof(leia_shaders_fullscreen_tri_vert), &vs);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck vs create failed: %d", res);
		return false;
	}
	res = ck_create_shader_module(vk, leia_shaders_ck_fill_frag,
	                              sizeof(leia_shaders_ck_fill_frag), &fs_fill);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck fill fs create failed: %d", res);
		vk->vkDestroyShaderModule(vk->device, vs, NULL);
		return false;
	}
	res = ck_create_shader_module(vk, leia_shaders_ck_strip_frag,
	                              sizeof(leia_shaders_ck_strip_frag), &fs_strip);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck strip fs create failed: %d", res);
		vk->vkDestroyShaderModule(vk->device, fs_fill, NULL);
		vk->vkDestroyShaderModule(vk->device, vs, NULL);
		return false;
	}

	VkPipelineVertexInputStateCreateInfo vi = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};
	VkPipelineInputAssemblyStateCreateInfo ia = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkPipelineViewportStateCreateInfo vps = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	    .viewportCount = 1,
	    .scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo rs = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	    .polygonMode = VK_POLYGON_MODE_FILL,
	    .cullMode = VK_CULL_MODE_NONE,
	    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	    .lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo ms = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineColorBlendAttachmentState ba = {
	    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo cb = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &ba,
	};
	VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynstate = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	    .dynamicStateCount = 2,
	    .pDynamicStates = dyn,
	};

	VkPipelineShaderStageCreateInfo stages[2][2] = {
	    {
	        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	         .stage = VK_SHADER_STAGE_VERTEX_BIT,
	         .module = vs,
	         .pName = "main"},
	        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	         .module = fs_fill,
	         .pName = "main"},
	    },
	    {
	        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	         .stage = VK_SHADER_STAGE_VERTEX_BIT,
	         .module = vs,
	         .pName = "main"},
	        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	         .module = fs_strip,
	         .pName = "main"},
	    },
	};

	VkGraphicsPipelineCreateInfo pis[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	        .stageCount = 2,
	        .pStages = stages[0],
	        .pVertexInputState = &vi,
	        .pInputAssemblyState = &ia,
	        .pViewportState = &vps,
	        .pRasterizationState = &rs,
	        .pMultisampleState = &ms,
	        .pColorBlendState = &cb,
	        .pDynamicState = &dynstate,
	        .layout = ldp->ck_pipeline_layout,
	        .renderPass = ldp->ck_fill_rp,
	        .subpass = 0,
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	        .stageCount = 2,
	        .pStages = stages[1],
	        .pVertexInputState = &vi,
	        .pInputAssemblyState = &ia,
	        .pViewportState = &vps,
	        .pRasterizationState = &rs,
	        .pMultisampleState = &ms,
	        .pColorBlendState = &cb,
	        .pDynamicState = &dynstate,
	        .layout = ldp->ck_pipeline_layout,
	        .renderPass = ldp->ck_strip_rp,
	        .subpass = 0,
	    },
	};
	VkPipeline out_pipes[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	res = vk->vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 2, pis, NULL, out_pipes);

	vk->vkDestroyShaderModule(vk->device, fs_strip, NULL);
	vk->vkDestroyShaderModule(vk->device, fs_fill, NULL);
	vk->vkDestroyShaderModule(vk->device, vs, NULL);

	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck pipelines create failed: %d", res);
		return false;
	}
	ldp->ck_fill_pipeline = out_pipes[0];
	ldp->ck_strip_pipeline = out_pipes[1];

	ldp->ck_inited = true;
	U_LOG_W("Leia VK DP: chroma-key pipelines initialized (key=0x%06x, target_fmt=%d)",
	        ldp->ck_color & 0x00FFFFFFu, (int)target_format);
	return true;
}

// Create or recreate the fill image+view+framebuffer to match (w, h).
static bool
ck_ensure_fill_target(struct leia_display_processor *ldp, uint32_t w, uint32_t h)
{
	if (ldp->ck_fill_image != VK_NULL_HANDLE && ldp->ck_fill_w == w && ldp->ck_fill_h == h) {
		return true;
	}

	struct vk_bundle *vk = ldp->vk;

	// Tear down existing.
	if (ldp->ck_fill_fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, ldp->ck_fill_fb, NULL);
		ldp->ck_fill_fb = VK_NULL_HANDLE;
	}
	if (ldp->ck_fill_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, ldp->ck_fill_view, NULL);
		ldp->ck_fill_view = VK_NULL_HANDLE;
	}
	if (ldp->ck_fill_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, ldp->ck_fill_image, NULL);
		ldp->ck_fill_image = VK_NULL_HANDLE;
	}
	if (ldp->ck_fill_mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, ldp->ck_fill_mem, NULL);
		ldp->ck_fill_mem = VK_NULL_HANDLE;
	}

	VkExtent2D ext = {w, h};
	VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	                          VK_IMAGE_USAGE_SAMPLED_BIT;
	VkResult res = vk_create_image_simple(
	    vk, ext, VK_FORMAT_R8G8B8A8_UNORM, usage,
	    &ldp->ck_fill_mem, &ldp->ck_fill_image);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck_fill_image create failed: %d", res);
		return false;
	}

	VkImageSubresourceRange sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	res = vk_create_view(vk, ldp->ck_fill_image, VK_IMAGE_VIEW_TYPE_2D,
	                     VK_FORMAT_R8G8B8A8_UNORM, sub, &ldp->ck_fill_view);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck_fill_view create failed: %d", res);
		return false;
	}

	VkFramebufferCreateInfo fbi = {
	    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
	    .renderPass = ldp->ck_fill_rp,
	    .attachmentCount = 1,
	    .pAttachments = &ldp->ck_fill_view,
	    .width = w,
	    .height = h,
	    .layers = 1,
	};
	res = vk->vkCreateFramebuffer(vk->device, &fbi, NULL, &ldp->ck_fill_fb);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck_fill_fb create failed: %d", res);
		return false;
	}

	ldp->ck_fill_w = w;
	ldp->ck_fill_h = h;
	return true;
}

// Create or recreate the strip sampling image+view to match (w, h).
static bool
ck_ensure_strip_source(struct leia_display_processor *ldp, uint32_t w, uint32_t h)
{
	if (ldp->ck_strip_image != VK_NULL_HANDLE && ldp->ck_strip_w == w && ldp->ck_strip_h == h) {
		return true;
	}

	struct vk_bundle *vk = ldp->vk;

	if (ldp->ck_strip_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, ldp->ck_strip_view, NULL);
		ldp->ck_strip_view = VK_NULL_HANDLE;
	}
	if (ldp->ck_strip_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, ldp->ck_strip_image, NULL);
		ldp->ck_strip_image = VK_NULL_HANDLE;
	}
	if (ldp->ck_strip_mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, ldp->ck_strip_mem, NULL);
		ldp->ck_strip_mem = VK_NULL_HANDLE;
	}

	VkExtent2D ext = {w, h};
	VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                          VK_IMAGE_USAGE_SAMPLED_BIT;
	// Use the swapchain target format for the sampling image so vkCmdCopyImage
	// is a straight format-matched copy.
	VkResult res = vk_create_image_simple(
	    vk, ext, ldp->ck_strip_target_format, usage,
	    &ldp->ck_strip_mem, &ldp->ck_strip_image);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck_strip_image create failed: %d", res);
		return false;
	}

	VkImageSubresourceRange sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	res = vk_create_view(vk, ldp->ck_strip_image, VK_IMAGE_VIEW_TYPE_2D,
	                     ldp->ck_strip_target_format, sub, &ldp->ck_strip_view);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck_strip_view create failed: %d", res);
		return false;
	}

	ldp->ck_strip_w = w;
	ldp->ck_strip_h = h;
	return true;
}

// Build a fresh VkImageView+VkFramebuffer for (target_image, w, h). Both are
// owned by the cache and torn down in ck_release_resources or when an entry
// is evicted. On failure both fb and view are NULL and false is returned.
static bool
ck_build_strip_entry(struct leia_display_processor *ldp,
                     VkImage target_image,
                     uint32_t w,
                     uint32_t h,
                     VkImageView *out_view,
                     VkFramebuffer *out_fb)
{
	struct vk_bundle *vk = ldp->vk;

	VkImageSubresourceRange sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageView view = VK_NULL_HANDLE;
	VkResult r = vk_create_view(vk, target_image, VK_IMAGE_VIEW_TYPE_2D,
	                            ldp->ck_strip_target_format, sub, &view);
	if (r != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck strip target view failed: %d", r);
		return false;
	}

	VkFramebufferCreateInfo fbi = {
	    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
	    .renderPass = ldp->ck_strip_rp,
	    .attachmentCount = 1,
	    .pAttachments = &view,
	    .width = w,
	    .height = h,
	    .layers = 1,
	};
	VkFramebuffer fb = VK_NULL_HANDLE;
	r = vk->vkCreateFramebuffer(vk->device, &fbi, NULL, &fb);
	if (r != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck strip fb create failed: %d", r);
		vk->vkDestroyImageView(vk->device, view, NULL);
		return false;
	}
	*out_view = view;
	*out_fb = fb;
	return true;
}

// Get (or lazily create) the strip output framebuffer for the given target
// image. Cached; the DP owns both the view and the framebuffer.
static VkFramebuffer
ck_get_strip_fb(struct leia_display_processor *ldp,
                VkImage target_image,
                uint32_t w,
                uint32_t h)
{
	struct vk_bundle *vk = ldp->vk;

	for (uint32_t i = 0; i < ldp->ck_strip_fbs_count; i++) {
		if (ldp->ck_strip_fbs[i].image == target_image) {
			if (ldp->ck_strip_fbs[i].w == w && ldp->ck_strip_fbs[i].h == h) {
				return ldp->ck_strip_fbs[i].fb;
			}
			// Dimensions changed — tear down and rebuild.
			vk->vkDestroyFramebuffer(vk->device, ldp->ck_strip_fbs[i].fb, NULL);
			vk->vkDestroyImageView(vk->device, ldp->ck_strip_fbs[i].view, NULL);
			VkImageView v = VK_NULL_HANDLE;
			VkFramebuffer f = VK_NULL_HANDLE;
			if (!ck_build_strip_entry(ldp, target_image, w, h, &v, &f)) {
				ldp->ck_strip_fbs[i].view = VK_NULL_HANDLE;
				ldp->ck_strip_fbs[i].fb = VK_NULL_HANDLE;
				return VK_NULL_HANDLE;
			}
			ldp->ck_strip_fbs[i].view = v;
			ldp->ck_strip_fbs[i].fb = f;
			ldp->ck_strip_fbs[i].w = w;
			ldp->ck_strip_fbs[i].h = h;
			return f;
		}
	}

	if (ldp->ck_strip_fbs_count >= kMaxStripFramebuffers) {
		U_LOG_W("Leia VK DP: ck strip fb cache full (%u entries) — dropping oldest",
		        ldp->ck_strip_fbs_count);
		vk->vkDestroyFramebuffer(vk->device, ldp->ck_strip_fbs[0].fb, NULL);
		vk->vkDestroyImageView(vk->device, ldp->ck_strip_fbs[0].view, NULL);
		for (uint32_t i = 1; i < ldp->ck_strip_fbs_count; i++) {
			ldp->ck_strip_fbs[i - 1] = ldp->ck_strip_fbs[i];
		}
		ldp->ck_strip_fbs_count--;
	}

	VkImageView v = VK_NULL_HANDLE;
	VkFramebuffer f = VK_NULL_HANDLE;
	if (!ck_build_strip_entry(ldp, target_image, w, h, &v, &f)) {
		return VK_NULL_HANDLE;
	}
	uint32_t idx = ldp->ck_strip_fbs_count++;
	ldp->ck_strip_fbs[idx].image = target_image;
	ldp->ck_strip_fbs[idx].view = v;
	ldp->ck_strip_fbs[idx].fb = f;
	ldp->ck_strip_fbs[idx].w = w;
	ldp->ck_strip_fbs[idx].h = h;
	return f;
}

// Pre-weave fill: render atlas RGBA -> ck_fill_image with alpha=0 pixels filled
// by the chroma key. Returns the ck_fill_view to be passed to the weaver as
// input. Caller must already have transitioned atlas_image to
// SHADER_READ_ONLY_OPTIMAL (the SR weaver expects the same).
//
// On failure returns VK_NULL_HANDLE; caller falls back to the original atlas.
static VkImageView
ck_run_pre_weave_fill(struct leia_display_processor *ldp,
                      VkCommandBuffer cmd,
                      VkImageView atlas_view,
                      uint32_t atlas_w,
                      uint32_t atlas_h)
{
	struct vk_bundle *vk = ldp->vk;

	if (!ck_ensure_fill_target(ldp, atlas_w, atlas_h)) return VK_NULL_HANDLE;

	// Update the fill descriptor set to point at the atlas.
	VkDescriptorImageInfo img_info = {
	    .sampler = ldp->ck_sampler,
	    .imageView = atlas_view,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet w = {
	    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	    .dstSet = ldp->ck_fill_set,
	    .dstBinding = 0,
	    .descriptorCount = 1,
	    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .pImageInfo = &img_info,
	};
	vk->vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);

	// Transition ck_fill_image (UNDEFINED or SHADER_READ on second+ frame) ->
	// COLOR_ATTACHMENT_OPTIMAL.
	VkImageMemoryBarrier pre = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, // contents discarded; loadOp=DONT_CARE
	    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .image = ldp->ck_fill_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    0, 0, NULL, 0, NULL, 1, &pre);

	VkRenderPassBeginInfo rpbi = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = ldp->ck_fill_rp,
	    .framebuffer = ldp->ck_fill_fb,
	    .renderArea = {{0, 0}, {atlas_w, atlas_h}},
	};
	vk->vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ldp->ck_fill_pipeline);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                             ldp->ck_pipeline_layout, 0, 1, &ldp->ck_fill_set, 0, NULL);

	float pc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	ck_unpack_chroma_rgb(ldp->ck_color, pc);
	vk->vkCmdPushConstants(cmd, ldp->ck_pipeline_layout,
	                        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);

	VkViewport vp = {
	    .x = 0.0f, .y = 0.0f,
	    .width = (float)atlas_w, .height = (float)atlas_h,
	    .minDepth = 0.0f, .maxDepth = 1.0f,
	};
	vk->vkCmdSetViewport(cmd, 0, 1, &vp);
	VkRect2D sc = {{0, 0}, {atlas_w, atlas_h}};
	vk->vkCmdSetScissor(cmd, 0, 1, &sc);

	vk->vkCmdDraw(cmd, 3, 1, 0, 0);
	vk->vkCmdEndRenderPass(cmd);

	// Transition fill image to SHADER_READ_ONLY_OPTIMAL so the weaver can sample it.
	VkImageMemoryBarrier post = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .image = ldp->ck_fill_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    0, 0, NULL, 0, NULL, 1, &post);

	return ldp->ck_fill_view;
}

// Post-weave strip: copy presented swapchain image to ck_strip_image, then
// render the strip pass back into the swapchain image with chroma-matching
// pixels set to alpha=0 (premultiplied for DWM).
//
// On entry: target_image is in PRESENT_SRC_KHR (post-weave finalLayout).
// On exit:  target_image is in PRESENT_SRC_KHR (strip render pass finalLayout).
static void
ck_run_post_weave_strip(struct leia_display_processor *ldp,
                        VkCommandBuffer cmd,
                        VkImage target_image,
                        uint32_t w, uint32_t h)
{
	struct vk_bundle *vk = ldp->vk;

	if (!ck_ensure_strip_source(ldp, w, h)) return;
	VkFramebuffer strip_fb = ck_get_strip_fb(ldp, target_image, w, h);
	if (strip_fb == VK_NULL_HANDLE) return;

	// Update strip descriptor set to point at ck_strip_view.
	VkDescriptorImageInfo img_info = {
	    .sampler = ldp->ck_sampler,
	    .imageView = ldp->ck_strip_view,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet wr = {
	    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	    .dstSet = ldp->ck_strip_set,
	    .dstBinding = 0,
	    .descriptorCount = 1,
	    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .pImageInfo = &img_info,
	};
	vk->vkUpdateDescriptorSets(vk->device, 1, &wr, 0, NULL);

	// 1) target PRESENT_SRC_KHR -> TRANSFER_SRC_OPTIMAL ;
	//    ck_strip_image UNDEFINED/SHADER_READ -> TRANSFER_DST_OPTIMAL.
	VkImageMemoryBarrier pre[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        .image = target_image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, // contents replaced
	        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        .image = ldp->ck_strip_image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    },
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    0, 0, NULL, 0, NULL, 2, pre);

	VkImageCopy cp = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .srcOffset = {0, 0, 0},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstOffset = {0, 0, 0},
	    .extent = {w, h, 1},
	};
	vk->vkCmdCopyImage(cmd,
	    target_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    ldp->ck_strip_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    1, &cp);

	// 2) target TRANSFER_SRC -> COLOR_ATTACHMENT_OPTIMAL (matches strip rp initialLayout)
	//    ck_strip_image TRANSFER_DST -> SHADER_READ_ONLY_OPTIMAL
	VkImageMemoryBarrier mid[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	        .image = target_image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .image = ldp->ck_strip_image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    },
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    0, 0, NULL, 0, NULL, 2, mid);

	// Strip render pass writes back into the swapchain image with finalLayout=PRESENT_SRC_KHR.
	VkRenderPassBeginInfo rpbi = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = ldp->ck_strip_rp,
	    .framebuffer = strip_fb,
	    .renderArea = {{0, 0}, {w, h}},
	};
	vk->vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ldp->ck_strip_pipeline);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                             ldp->ck_pipeline_layout, 0, 1, &ldp->ck_strip_set, 0, NULL);

	float pc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	ck_unpack_chroma_rgb(ldp->ck_color, pc);
	vk->vkCmdPushConstants(cmd, ldp->ck_pipeline_layout,
	                        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);

	VkViewport vp = {
	    .x = 0.0f, .y = 0.0f,
	    .width = (float)w, .height = (float)h,
	    .minDepth = 0.0f, .maxDepth = 1.0f,
	};
	vk->vkCmdSetViewport(cmd, 0, 1, &vp);
	VkRect2D sc = {{0, 0}, {w, h}};
	vk->vkCmdSetScissor(cmd, 0, 1, &sc);

	vk->vkCmdDraw(cmd, 3, 1, 0, 0);
	vk->vkCmdEndRenderPass(cmd);
}

static void
ck_release_resources(struct leia_display_processor *ldp)
{
	if (ldp == NULL || ldp->vk == NULL) return;
	struct vk_bundle *vk = ldp->vk;

	for (uint32_t i = 0; i < ldp->ck_strip_fbs_count; i++) {
		if (ldp->ck_strip_fbs[i].fb != VK_NULL_HANDLE) {
			vk->vkDestroyFramebuffer(vk->device, ldp->ck_strip_fbs[i].fb, NULL);
		}
		if (ldp->ck_strip_fbs[i].view != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, ldp->ck_strip_fbs[i].view, NULL);
		}
	}
	ldp->ck_strip_fbs_count = 0;

	if (ldp->ck_fill_fb != VK_NULL_HANDLE)   vk->vkDestroyFramebuffer(vk->device, ldp->ck_fill_fb, NULL);
	if (ldp->ck_fill_view != VK_NULL_HANDLE) vk->vkDestroyImageView(vk->device, ldp->ck_fill_view, NULL);
	if (ldp->ck_fill_image != VK_NULL_HANDLE) vk->vkDestroyImage(vk->device, ldp->ck_fill_image, NULL);
	if (ldp->ck_fill_mem != VK_NULL_HANDLE)  vk->vkFreeMemory(vk->device, ldp->ck_fill_mem, NULL);

	if (ldp->ck_strip_view != VK_NULL_HANDLE) vk->vkDestroyImageView(vk->device, ldp->ck_strip_view, NULL);
	if (ldp->ck_strip_image != VK_NULL_HANDLE) vk->vkDestroyImage(vk->device, ldp->ck_strip_image, NULL);
	if (ldp->ck_strip_mem != VK_NULL_HANDLE)  vk->vkFreeMemory(vk->device, ldp->ck_strip_mem, NULL);

	if (ldp->ck_fill_pipeline != VK_NULL_HANDLE)  vk->vkDestroyPipeline(vk->device, ldp->ck_fill_pipeline, NULL);
	if (ldp->ck_strip_pipeline != VK_NULL_HANDLE) vk->vkDestroyPipeline(vk->device, ldp->ck_strip_pipeline, NULL);
	if (ldp->ck_pipeline_layout != VK_NULL_HANDLE) vk->vkDestroyPipelineLayout(vk->device, ldp->ck_pipeline_layout, NULL);
	if (ldp->ck_desc_pool != VK_NULL_HANDLE) vk->vkDestroyDescriptorPool(vk->device, ldp->ck_desc_pool, NULL);
	if (ldp->ck_desc_layout != VK_NULL_HANDLE) vk->vkDestroyDescriptorSetLayout(vk->device, ldp->ck_desc_layout, NULL);
	if (ldp->ck_sampler != VK_NULL_HANDLE) vk->vkDestroySampler(vk->device, ldp->ck_sampler, NULL);
	if (ldp->ck_fill_rp != VK_NULL_HANDLE) vk->vkDestroyRenderPass(vk->device, ldp->ck_fill_rp, NULL);
	if (ldp->ck_strip_rp != VK_NULL_HANDLE) vk->vkDestroyRenderPass(vk->device, ldp->ck_strip_rp, NULL);

	std::memset(&ldp->ck_strip_fbs, 0, sizeof(ldp->ck_strip_fbs));
	ldp->ck_fill_fb = VK_NULL_HANDLE;
	ldp->ck_fill_view = VK_NULL_HANDLE;
	ldp->ck_fill_image = VK_NULL_HANDLE;
	ldp->ck_fill_mem = VK_NULL_HANDLE;
	ldp->ck_strip_view = VK_NULL_HANDLE;
	ldp->ck_strip_image = VK_NULL_HANDLE;
	ldp->ck_strip_mem = VK_NULL_HANDLE;
	ldp->ck_fill_pipeline = VK_NULL_HANDLE;
	ldp->ck_strip_pipeline = VK_NULL_HANDLE;
	ldp->ck_pipeline_layout = VK_NULL_HANDLE;
	ldp->ck_desc_pool = VK_NULL_HANDLE;
	ldp->ck_desc_layout = VK_NULL_HANDLE;
	ldp->ck_sampler = VK_NULL_HANDLE;
	ldp->ck_fill_rp = VK_NULL_HANDLE;
	ldp->ck_strip_rp = VK_NULL_HANDLE;
	ldp->ck_inited = false;
}


/*
 *
 * Compose-under-bg pipeline (preferred over chroma-key on Windows).
 *
 * Reuses ck_fill_image / ck_fill_view / ck_fill_fb / ck_fill_rp as the
 * intermediate target — ck_ensure_fill_target + ck_init_pipeline still
 * create those. Distinct pipeline + descriptor set with 2 SRVs.
 *
 * Cross-API sync note: WGC capture (~60 Hz, sub-ms copy) is much slower
 * than the consumer's render rate (~120 Hz). The producer's D3D11 context
 * is Flushed after every Copy. Without a GPU-level semaphore wait, the
 * consumer may very occasionally see a torn frame mid-copy; in practice
 * the temporal gap dwarfs the copy duration. Proper VK_KHR_external_
 * semaphore_win32 import via D3D12_FENCE handle type is a follow-up.
 *
 */

#ifdef _WIN32

static bool
compose_should_run(struct leia_display_processor *ldp)
{
	return ldp->bg_compose_enabled && ldp->bg_capture != nullptr && ldp->bg_view != VK_NULL_HANDLE;
}

// Import the bg_capture's shared NT-handle texture as VkImage + VkImageView.
// One-shot on session start; the imported memory's lifetime is tied to the DP.
static bool
compose_import_bg_image(struct leia_display_processor *ldp)
{
	struct vk_bundle *vk = ldp->vk;
	HANDLE handle = leia_bg_capture_get_shared_handle(ldp->bg_capture);
	if (handle == nullptr) {
		U_LOG_W("Leia VK DP: bg_capture has no shared handle");
		return false;
	}
	leia_bg_capture_get_size(ldp->bg_capture, &ldp->bg_w, &ldp->bg_h);
	if (ldp->bg_w == 0 || ldp->bg_h == 0) {
		U_LOG_W("Leia VK DP: bg_capture has zero size");
		return false;
	}

	// NT-handle import path: VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT.
	// Format must match the D3D11 staging tex (BGRA8).
	VkExternalMemoryImageCreateInfo ext_ci = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
	};
	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &ext_ci,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .extent = {ldp->bg_w, ldp->bg_h, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkResult res = vk->vkCreateImage(vk->device, &image_ci, NULL, &ldp->bg_image);
	if (res != VK_SUCCESS) {
		U_LOG_W("Leia VK DP: bg vkCreateImage failed: %d", res);
		return false;
	}

	VkMemoryRequirements reqs = {};
	vk->vkGetImageMemoryRequirements(vk->device, ldp->bg_image, &reqs);

	VkImportMemoryWin32HandleInfoKHR import = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
	    .handle = handle,
	};
	VkMemoryDedicatedAllocateInfoKHR dedicated = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
	    .pNext = &import,
	    .image = ldp->bg_image,
	};
	VkPhysicalDeviceMemoryProperties mp = {};
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mp);
	uint32_t mti = UINT32_MAX;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		if ((reqs.memoryTypeBits & (1u << i)) != 0) {
			mti = i;
			break;
		}
	}
	if (mti == UINT32_MAX) {
		U_LOG_W("Leia VK DP: no memory type for bg import");
		vk->vkDestroyImage(vk->device, ldp->bg_image, NULL);
		ldp->bg_image = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryAllocateInfo alloc = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &dedicated,
	    .allocationSize = reqs.size,
	    .memoryTypeIndex = mti,
	};
	res = vk->vkAllocateMemory(vk->device, &alloc, NULL, &ldp->bg_mem);
	if (res != VK_SUCCESS) {
		U_LOG_W("Leia VK DP: bg vkAllocateMemory failed: %d", res);
		vk->vkDestroyImage(vk->device, ldp->bg_image, NULL);
		ldp->bg_image = VK_NULL_HANDLE;
		return false;
	}
	res = vk->vkBindImageMemory(vk->device, ldp->bg_image, ldp->bg_mem, 0);
	if (res != VK_SUCCESS) {
		U_LOG_W("Leia VK DP: bg vkBindImageMemory failed: %d", res);
		vk->vkFreeMemory(vk->device, ldp->bg_mem, NULL);
		vk->vkDestroyImage(vk->device, ldp->bg_image, NULL);
		ldp->bg_mem = VK_NULL_HANDLE;
		ldp->bg_image = VK_NULL_HANDLE;
		return false;
	}

	VkImageViewCreateInfo vi = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = ldp->bg_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	res = vk->vkCreateImageView(vk->device, &vi, NULL, &ldp->bg_view);
	if (res != VK_SUCCESS) {
		U_LOG_W("Leia VK DP: bg vkCreateImageView failed: %d", res);
		vk->vkFreeMemory(vk->device, ldp->bg_mem, NULL);
		vk->vkDestroyImage(vk->device, ldp->bg_image, NULL);
		ldp->bg_mem = VK_NULL_HANDLE;
		ldp->bg_image = VK_NULL_HANDLE;
		return false;
	}

	U_LOG_W("Leia VK DP: bg D3D11 texture imported (%ux%u)", ldp->bg_w, ldp->bg_h);
	return true;
}

// Build compose-specific descriptor layout / pipeline layout / sampler /
// descriptor pool / pipeline. Reuses ck_fill_rp as the render pass (same
// R8G8B8A8_UNORM attachment). Requires ck_init_pipeline to have run first
// so ck_fill_rp exists. Idempotent.
static bool
compose_init_pipeline(struct leia_display_processor *ldp)
{
	if (ldp->compose_inited) return true;
	if (ldp->ck_fill_rp == VK_NULL_HANDLE) {
		U_LOG_W("Leia VK DP: compose_init_pipeline before ck_fill_rp exists");
		return false;
	}

	struct vk_bundle *vk = ldp->vk;
	VkResult res;

	// 2-binding descriptor set: atlas (0) + bg (1).
	{
		VkDescriptorSetLayoutBinding bs[2] = {
		    {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		     .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
		    {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		     .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
		};
		VkDescriptorSetLayoutCreateInfo ci = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		    .bindingCount = 2, .pBindings = bs,
		};
		res = vk->vkCreateDescriptorSetLayout(vk->device, &ci, NULL, &ldp->compose_desc_layout);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: compose desc layout failed: %d", res);
			return false;
		}
	}

	// Push constants: 2*vec2 + uvec2 + uvec2 pad = 32 bytes (no more chroma_rgb
	// — alpha-gate handles transparency holes, not the compose shader).
	{
		VkPushConstantRange pc = {
		    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		    .offset = 0,
		    .size = 32,
		};
		VkPipelineLayoutCreateInfo pli = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		    .setLayoutCount = 1, .pSetLayouts = &ldp->compose_desc_layout,
		    .pushConstantRangeCount = 1, .pPushConstantRanges = &pc,
		};
		res = vk->vkCreatePipelineLayout(vk->device, &pli, NULL, &ldp->compose_pipeline_layout);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: compose pipeline layout failed: %d", res);
			return false;
		}
	}

	// Linear sampler.
	{
		VkSamplerCreateInfo si = {
		    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		    .magFilter = VK_FILTER_LINEAR,
		    .minFilter = VK_FILTER_LINEAR,
		    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
		    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		    .maxLod = 1.0f,
		};
		res = vk->vkCreateSampler(vk->device, &si, NULL, &ldp->compose_sampler);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: compose sampler failed: %d", res);
			return false;
		}
	}

	// Descriptor pool + 1 set.
	{
		VkDescriptorPoolSize size = {
		    .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		    .descriptorCount = 2,
		};
		VkDescriptorPoolCreateInfo dpi = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		    .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size,
		};
		res = vk->vkCreateDescriptorPool(vk->device, &dpi, NULL, &ldp->compose_desc_pool);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: compose desc pool failed: %d", res);
			return false;
		}
		VkDescriptorSetAllocateInfo ai = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		    .descriptorPool = ldp->compose_desc_pool,
		    .descriptorSetCount = 1,
		    .pSetLayouts = &ldp->compose_desc_layout,
		};
		res = vk->vkAllocateDescriptorSets(vk->device, &ai, &ldp->compose_set);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: compose desc set alloc failed: %d", res);
			return false;
		}
	}

	// Bind the bg view to slot 1 once — it doesn't change across frames.
	{
		VkDescriptorImageInfo info = {
		    .sampler = ldp->compose_sampler,
		    .imageView = ldp->bg_view,
		    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		VkWriteDescriptorSet w = {
		    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		    .dstSet = ldp->compose_set,
		    .dstBinding = 1,
		    .descriptorCount = 1,
		    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		    .pImageInfo = &info,
		};
		vk->vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);
	}

	// Build pipeline.
	VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
	res = ck_create_shader_module(vk, leia_shaders_fullscreen_tri_vert,
	                              sizeof(leia_shaders_fullscreen_tri_vert), &vs);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: compose vs create failed: %d", res);
		return false;
	}
	res = ck_create_shader_module(vk, leia_shaders_compose_under_bg_frag,
	                              sizeof(leia_shaders_compose_under_bg_frag), &fs);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: compose fs create failed: %d", res);
		vk->vkDestroyShaderModule(vk->device, vs, NULL);
		return false;
	}

	VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkPipelineViewportStateCreateInfo vps = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	    .viewportCount = 1, .scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo rs = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	    .polygonMode = VK_POLYGON_MODE_FILL,
	    .cullMode = VK_CULL_MODE_NONE,
	    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	    .lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo ms = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineColorBlendAttachmentState ba = {
	    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo cb = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	    .attachmentCount = 1, .pAttachments = &ba,
	};
	VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynstate = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	    .dynamicStateCount = 2, .pDynamicStates = dyn,
	};
	VkPipelineShaderStageCreateInfo stages[2] = {
	    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	     .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
	    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	     .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
	};
	VkGraphicsPipelineCreateInfo pi = {
	    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	    .stageCount = 2, .pStages = stages,
	    .pVertexInputState = &vi,
	    .pInputAssemblyState = &ia,
	    .pViewportState = &vps,
	    .pRasterizationState = &rs,
	    .pMultisampleState = &ms,
	    .pColorBlendState = &cb,
	    .pDynamicState = &dynstate,
	    .layout = ldp->compose_pipeline_layout,
	    .renderPass = ldp->ck_fill_rp,
	    .subpass = 0,
	};
	res = vk->vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pi, NULL, &ldp->compose_pipeline);
	vk->vkDestroyShaderModule(vk->device, fs, NULL);
	vk->vkDestroyShaderModule(vk->device, vs, NULL);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: compose pipeline create failed: %d", res);
		return false;
	}

	// Alpha-gate pipeline — pairs with compose pass. Reuses ck_strip_rp as
	// the output render pass (writes swap-chain image, finalLayout =
	// PRESENT_SRC_KHR), and the ck_strip_image / ck_strip_view backbuffer
	// copy plumbing. Distinct descriptor set (2 image samplers — backbuffer
	// copy + atlas) and push constants (tile_count).
	if (ldp->alpha_gate_pipeline == VK_NULL_HANDLE) {
		// 2-binding descriptor set.
		{
			VkDescriptorSetLayoutBinding bs[2] = {
			    {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			     .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
			    {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			     .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
			};
			VkDescriptorSetLayoutCreateInfo ci = {
			    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			    .bindingCount = 2, .pBindings = bs,
			};
			res = vk->vkCreateDescriptorSetLayout(vk->device, &ci, NULL, &ldp->alpha_gate_desc_layout);
			if (res != VK_SUCCESS) {
				U_LOG_E("Leia VK DP: alpha-gate desc layout failed: %d", res);
				return false;
			}
		}
		// Push constants: uvec2 tile_count + uvec2 pad = 16 bytes.
		{
			VkPushConstantRange pc = {
			    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			    .offset = 0,
			    .size = 16,
			};
			VkPipelineLayoutCreateInfo pli = {
			    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount = 1, .pSetLayouts = &ldp->alpha_gate_desc_layout,
			    .pushConstantRangeCount = 1, .pPushConstantRanges = &pc,
			};
			res = vk->vkCreatePipelineLayout(vk->device, &pli, NULL, &ldp->alpha_gate_pipeline_layout);
			if (res != VK_SUCCESS) {
				U_LOG_E("Leia VK DP: alpha-gate pipeline layout failed: %d", res);
				return false;
			}
		}
		// Descriptor pool + 1 set.
		{
			VkDescriptorPoolSize size = {
			    .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			    .descriptorCount = 2,
			};
			VkDescriptorPoolCreateInfo dpi = {
			    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			    .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size,
			};
			res = vk->vkCreateDescriptorPool(vk->device, &dpi, NULL, &ldp->alpha_gate_desc_pool);
			if (res != VK_SUCCESS) {
				U_LOG_E("Leia VK DP: alpha-gate desc pool failed: %d", res);
				return false;
			}
			VkDescriptorSetAllocateInfo ai = {
			    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .descriptorPool = ldp->alpha_gate_desc_pool,
			    .descriptorSetCount = 1,
			    .pSetLayouts = &ldp->alpha_gate_desc_layout,
			};
			res = vk->vkAllocateDescriptorSets(vk->device, &ai, &ldp->alpha_gate_set);
			if (res != VK_SUCCESS) {
				U_LOG_E("Leia VK DP: alpha-gate desc set alloc failed: %d", res);
				return false;
			}
		}
		// Build pipeline against ck_strip_rp (writes swap-chain image).
		VkShaderModule ag_vs = VK_NULL_HANDLE, ag_fs = VK_NULL_HANDLE;
		res = ck_create_shader_module(vk, leia_shaders_fullscreen_tri_vert,
		                              sizeof(leia_shaders_fullscreen_tri_vert), &ag_vs);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: alpha-gate vs create failed: %d", res);
			return false;
		}
		res = ck_create_shader_module(vk, leia_shaders_alpha_gate_frag,
		                              sizeof(leia_shaders_alpha_gate_frag), &ag_fs);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: alpha-gate fs create failed: %d", res);
			vk->vkDestroyShaderModule(vk->device, ag_vs, NULL);
			return false;
		}

		VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo ia = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		};
		VkPipelineViewportStateCreateInfo vps = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		    .viewportCount = 1, .scissorCount = 1,
		};
		VkPipelineRasterizationStateCreateInfo rs = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		    .polygonMode = VK_POLYGON_MODE_FILL,
		    .cullMode = VK_CULL_MODE_NONE,
		    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		    .lineWidth = 1.0f,
		};
		VkPipelineMultisampleStateCreateInfo ms = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		};
		VkPipelineColorBlendAttachmentState ba = {
		    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
		};
		VkPipelineColorBlendStateCreateInfo cb = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		    .attachmentCount = 1, .pAttachments = &ba,
		};
		VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dynstate = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		    .dynamicStateCount = 2, .pDynamicStates = dyn,
		};
		VkPipelineShaderStageCreateInfo stages[2] = {
		    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		     .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = ag_vs, .pName = "main"},
		    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		     .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = ag_fs, .pName = "main"},
		};
		VkGraphicsPipelineCreateInfo pi = {
		    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		    .stageCount = 2, .pStages = stages,
		    .pVertexInputState = &vi,
		    .pInputAssemblyState = &ia,
		    .pViewportState = &vps,
		    .pRasterizationState = &rs,
		    .pMultisampleState = &ms,
		    .pColorBlendState = &cb,
		    .pDynamicState = &dynstate,
		    .layout = ldp->alpha_gate_pipeline_layout,
		    .renderPass = ldp->ck_strip_rp,
		    .subpass = 0,
		};
		res = vk->vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pi, NULL, &ldp->alpha_gate_pipeline);
		vk->vkDestroyShaderModule(vk->device, ag_fs, NULL);
		vk->vkDestroyShaderModule(vk->device, ag_vs, NULL);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: alpha-gate pipeline create failed: %d", res);
			return false;
		}
	}

	ldp->compose_inited = true;
	U_LOG_W("Leia VK DP: compose-under-bg + alpha-gate pipelines ready");
	return true;
}

// Pre-weave compose. Polls WGC, transitions bg → SHADER_READ once, renders the
// compose pass into ck_fill_image. Returns ck_fill_view for the weaver.
static VkImageView
compose_run_pre_weave(struct leia_display_processor *ldp,
                      VkCommandBuffer cmd,
                      VkImageView atlas_view,
                      uint32_t atlas_w,
                      uint32_t atlas_h,
                      uint32_t tile_columns,
                      uint32_t tile_rows)
{
	struct vk_bundle *vk = ldp->vk;
	if (!ck_ensure_fill_target(ldp, atlas_w, atlas_h)) return VK_NULL_HANDLE;
	if (!compose_init_pipeline(ldp)) return VK_NULL_HANDLE;

	float bg_origin[2] = {0.0f, 0.0f};
	float bg_extent[2] = {0.0f, 0.0f};
	uint64_t unused = 0;
	bool have_bg = leia_bg_capture_poll(ldp->bg_capture, bg_origin, bg_extent, &unused);
	if (!have_bg) {
		return VK_NULL_HANDLE;
	}

	// First-use barrier on the imported bg image: UNDEFINED → SHADER_READ_ONLY.
	// Subsequent frames keep it in SHADER_READ_ONLY (the D3D11 producer writes
	// through the shared memory without going through VK layouts).
	static thread_local bool bg_transitioned = false;
	if (!bg_transitioned) {
		VkImageMemoryBarrier b = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .image = ldp->bg_image,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		vk->vkCmdPipelineBarrier(cmd,
		    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		    0, 0, NULL, 0, NULL, 1, &b);
		bg_transitioned = true;
	}

	// Refresh atlas SRV in compose set (slot 0). bg SRV (slot 1) is stable.
	VkDescriptorImageInfo atlas_info = {
	    .sampler = ldp->compose_sampler,
	    .imageView = atlas_view,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet w = {
	    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	    .dstSet = ldp->compose_set,
	    .dstBinding = 0,
	    .descriptorCount = 1,
	    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .pImageInfo = &atlas_info,
	};
	vk->vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);

	// ck_fill_image UNDEFINED → COLOR_ATTACHMENT.
	VkImageMemoryBarrier pre = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .image = ldp->ck_fill_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    0, 0, NULL, 0, NULL, 1, &pre);

	VkRenderPassBeginInfo rpbi = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = ldp->ck_fill_rp,
	    .framebuffer = ldp->ck_fill_fb,
	    .renderArea = {{0, 0}, {atlas_w, atlas_h}},
	};
	vk->vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ldp->compose_pipeline);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                             ldp->compose_pipeline_layout, 0, 1, &ldp->compose_set, 0, NULL);

	struct {
		float bg_uv_origin[2];
		float bg_uv_extent[2];
		uint32_t tile_count[2];
		uint32_t pad[2];
	} push = {};
	push.bg_uv_origin[0] = bg_origin[0]; push.bg_uv_origin[1] = bg_origin[1];
	push.bg_uv_extent[0] = bg_extent[0]; push.bg_uv_extent[1] = bg_extent[1];
	push.tile_count[0] = tile_columns;   push.tile_count[1] = tile_rows;
	vk->vkCmdPushConstants(cmd, ldp->compose_pipeline_layout,
	                        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

	VkViewport vp = {0.0f, 0.0f, (float)atlas_w, (float)atlas_h, 0.0f, 1.0f};
	VkRect2D sc = {{0, 0}, {atlas_w, atlas_h}};
	vk->vkCmdSetViewport(cmd, 0, 1, &vp);
	vk->vkCmdSetScissor(cmd, 0, 1, &sc);

	vk->vkCmdDraw(cmd, 3, 1, 0, 0);
	vk->vkCmdEndRenderPass(cmd);

	// ck_fill_image COLOR_ATTACHMENT → SHADER_READ for weaver.
	VkImageMemoryBarrier post = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .image = ldp->ck_fill_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    0, 0, NULL, 0, NULL, 1, &post);

	return ldp->ck_fill_view;
}

/*
 * Post-weave alpha-gate. Mirrors ck_run_post_weave_strip's backbuffer copy
 * dance but binds 2 SRVs (strip_view = back-buffer copy, atlas_view) and
 * the alpha-gate pipeline. Output is premultiplied RGBA — pixels matching
 * the "all views α==0" mask get (0,0,0,0); others get woven RGB at α=1.
 *
 * On entry: target_image is in PRESENT_SRC_KHR. On exit: same.
 */
static void
alpha_gate_run_post_weave(struct leia_display_processor *ldp,
                          VkCommandBuffer cmd,
                          VkImage target_image,
                          VkImageView atlas_view,
                          uint32_t w, uint32_t h,
                          uint32_t tile_columns, uint32_t tile_rows)
{
	struct vk_bundle *vk = ldp->vk;

	if (!ck_ensure_strip_source(ldp, w, h)) return;
	VkFramebuffer strip_fb = ck_get_strip_fb(ldp, target_image, w, h);
	if (strip_fb == VK_NULL_HANDLE) return;

	// Update alpha-gate descriptor set: slot 0 = strip_view (backbuffer copy),
	// slot 1 = atlas_view.
	VkDescriptorImageInfo infos[2] = {
	    {.sampler = ldp->compose_sampler, .imageView = ldp->ck_strip_view,
	     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
	    {.sampler = ldp->compose_sampler, .imageView = atlas_view,
	     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
	};
	VkWriteDescriptorSet writes[2] = {
	    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	     .dstSet = ldp->alpha_gate_set, .dstBinding = 0, .descriptorCount = 1,
	     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	     .pImageInfo = &infos[0]},
	    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	     .dstSet = ldp->alpha_gate_set, .dstBinding = 1, .descriptorCount = 1,
	     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	     .pImageInfo = &infos[1]},
	};
	vk->vkUpdateDescriptorSets(vk->device, 2, writes, 0, NULL);

	// target PRESENT_SRC_KHR → TRANSFER_SRC; strip_image UNDEFINED/SR → TRANSFER_DST.
	VkImageMemoryBarrier pre[2] = {
	    {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	     .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	     .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	     .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	     .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	     .image = target_image,
	     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	    {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	     .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	     .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	     .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	     .image = ldp->ck_strip_image,
	     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    0, 0, NULL, 0, NULL, 2, pre);

	VkImageCopy region = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .extent = {w, h, 1},
	};
	vk->vkCmdCopyImage(cmd,
	    target_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    ldp->ck_strip_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    1, &region);

	// target TRANSFER_SRC → COLOR_ATTACHMENT (will be written by the gate
	// draw via ck_strip_rp); strip_image TRANSFER_DST → SHADER_READ.
	VkImageMemoryBarrier mid[2] = {
	    {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	     .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	     .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	     .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	     .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	     .image = target_image,
	     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	    {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	     .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	     .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	     .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	     .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	     .image = ldp->ck_strip_image,
	     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    0, 0, NULL, 0, NULL, 2, mid);

	VkRenderPassBeginInfo rpbi = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = ldp->ck_strip_rp,
	    .framebuffer = strip_fb,
	    .renderArea = {{0, 0}, {w, h}},
	};
	vk->vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ldp->alpha_gate_pipeline);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                             ldp->alpha_gate_pipeline_layout, 0, 1, &ldp->alpha_gate_set, 0, NULL);

	struct { uint32_t tile_count[2]; uint32_t pad[2]; } push = {};
	push.tile_count[0] = tile_columns;
	push.tile_count[1] = tile_rows;
	vk->vkCmdPushConstants(cmd, ldp->alpha_gate_pipeline_layout,
	                        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

	VkViewport vp = {0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
	VkRect2D sc = {{0, 0}, {w, h}};
	vk->vkCmdSetViewport(cmd, 0, 1, &vp);
	vk->vkCmdSetScissor(cmd, 0, 1, &sc);

	vk->vkCmdDraw(cmd, 3, 1, 0, 0);
	vk->vkCmdEndRenderPass(cmd);
}

static void
compose_release_resources(struct leia_display_processor *ldp)
{
	if (ldp == NULL || ldp->vk == NULL) return;
	struct vk_bundle *vk = ldp->vk;

	if (ldp->alpha_gate_pipeline != VK_NULL_HANDLE) {
		vk->vkDestroyPipeline(vk->device, ldp->alpha_gate_pipeline, NULL);
		ldp->alpha_gate_pipeline = VK_NULL_HANDLE;
	}
	if (ldp->alpha_gate_desc_pool != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorPool(vk->device, ldp->alpha_gate_desc_pool, NULL);
		ldp->alpha_gate_desc_pool = VK_NULL_HANDLE;
	}
	if (ldp->alpha_gate_pipeline_layout != VK_NULL_HANDLE) {
		vk->vkDestroyPipelineLayout(vk->device, ldp->alpha_gate_pipeline_layout, NULL);
		ldp->alpha_gate_pipeline_layout = VK_NULL_HANDLE;
	}
	if (ldp->alpha_gate_desc_layout != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorSetLayout(vk->device, ldp->alpha_gate_desc_layout, NULL);
		ldp->alpha_gate_desc_layout = VK_NULL_HANDLE;
	}

	if (ldp->compose_pipeline != VK_NULL_HANDLE) {
		vk->vkDestroyPipeline(vk->device, ldp->compose_pipeline, NULL);
		ldp->compose_pipeline = VK_NULL_HANDLE;
	}
	if (ldp->compose_desc_pool != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorPool(vk->device, ldp->compose_desc_pool, NULL);
		ldp->compose_desc_pool = VK_NULL_HANDLE;
	}
	if (ldp->compose_pipeline_layout != VK_NULL_HANDLE) {
		vk->vkDestroyPipelineLayout(vk->device, ldp->compose_pipeline_layout, NULL);
		ldp->compose_pipeline_layout = VK_NULL_HANDLE;
	}
	if (ldp->compose_desc_layout != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorSetLayout(vk->device, ldp->compose_desc_layout, NULL);
		ldp->compose_desc_layout = VK_NULL_HANDLE;
	}
	if (ldp->compose_sampler != VK_NULL_HANDLE) {
		vk->vkDestroySampler(vk->device, ldp->compose_sampler, NULL);
		ldp->compose_sampler = VK_NULL_HANDLE;
	}
	if (ldp->bg_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, ldp->bg_view, NULL);
		ldp->bg_view = VK_NULL_HANDLE;
	}
	if (ldp->bg_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, ldp->bg_image, NULL);
		ldp->bg_image = VK_NULL_HANDLE;
	}
	if (ldp->bg_mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, ldp->bg_mem, NULL);
		ldp->bg_mem = VK_NULL_HANDLE;
	}
	if (ldp->bg_capture != nullptr) {
		leia_bg_capture_destroy(ldp->bg_capture);
		ldp->bg_capture = nullptr;
	}
	ldp->bg_compose_enabled = false;
	ldp->compose_inited = false;
}

#else // !_WIN32

static inline bool compose_should_run(struct leia_display_processor *) { return false; }
static inline void compose_release_resources(struct leia_display_processor *) {}

#endif // _WIN32


/*
 *
 * xrt_display_processor interface methods.
 *
 */

static void
leia_dp_process_atlas(struct xrt_display_processor *xdp,
                      VkCommandBuffer cmd_buffer,
                      VkImage_XDP atlas_image,
                      VkImageView atlas_view,
                      uint32_t view_width,
                      uint32_t view_height,
                      uint32_t tile_columns,
                      uint32_t tile_rows,
                      VkFormat_XDP view_format,
                      VkFramebuffer target_fb,
                      VkImage_XDP target_image,
                      uint32_t target_width,
                      uint32_t target_height,
                      VkFormat_XDP target_format,
                      int32_t canvas_offset_x,
                      int32_t canvas_offset_y,
                      uint32_t canvas_width,
                      uint32_t canvas_height)
{
	// TODO(#85): Pass canvas_offset_x/y to vendor weaver for interlacing
	// phase correction once Leia SR SDK supports sub-rect offset.
	(void)canvas_offset_x;
	(void)canvas_offset_y;
	(void)canvas_width;
	(void)canvas_height;

	struct leia_display_processor *ldp = leia_display_processor(xdp);
	struct vk_bundle *vk = ldp->vk;

	// 2D mode: bypass weaver, blit atlas content directly to target
	if (ldp->view_count == 1 && target_image != (VkImage_XDP)VK_NULL_HANDLE) {
		// Barrier: atlas SHADER_READ → TRANSFER_SRC, target COLOR_ATTACHMENT → TRANSFER_DST
		VkImageMemoryBarrier pre[2] = {
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .image = (VkImage)atlas_image,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		    },
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .image = (VkImage)target_image,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		    },
		};
		vk->vkCmdPipelineBarrier(cmd_buffer,
		    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT,
		    0, 0, NULL, 0, NULL, 2, pre);

		// Blit atlas content region (single view) to full target
		VkImageBlit blit = {
		    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		    .srcOffsets = {{0, 0, 0}, {(int32_t)view_width, (int32_t)view_height, 1}},
		    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		    .dstOffsets = {{0, 0, 0}, {(int32_t)target_width, (int32_t)target_height, 1}},
		};
		vk->vkCmdBlitImage(cmd_buffer,
		    (VkImage)atlas_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    (VkImage)target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    1, &blit, VK_FILTER_LINEAR);

		// Barrier: restore atlas → SHADER_READ, target → COLOR_ATTACHMENT_OPTIMAL
		VkImageMemoryBarrier post[2] = {
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .image = (VkImage)atlas_image,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		    },
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		        .image = (VkImage)target_image,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		    },
		};
		vk->vkCmdPipelineBarrier(cmd_buffer,
		    VK_PIPELINE_STAGE_TRANSFER_BIT,
		    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		    0, 0, NULL, 0, NULL, 2, post);
		return;
	}

	// Atlas is guaranteed content-sized SBS (2*view_width x view_height)
	// by compositor crop-blit. Pass directly to weaver.

	// Build a fullscreen viewport from target dimensions.
	VkRect2D viewport = {};
	viewport.offset.x = 0;
	viewport.offset.y = 0;
	viewport.extent.width = target_width;
	viewport.extent.height = target_height;

	// Transparency support — two paths:
	//
	//   1. Compose-under-bg (preferred, Windows + WGC): pre-composite the
	//      captured desktop region UNDER each per-view atlas tile so the
	//      weaver consumes opaque RGB with the desktop already integrated.
	//      No post-weave strip needed. Activated by set_chroma_key when WGC
	//      init succeeded. Correct on AA edges and semi-transparent pixels.
	//
	//   2. Chroma-key (fallback): replace alpha=0 with key color pre-weave,
	//      strip back to alpha=0 post-weave. Hard mask on AA edges. Used
	//      when WGC is unavailable.
	VkImageView weaver_input = atlas_view;
	bool ck_active = false;
	uint32_t atlas_w = view_width * tile_columns;
	uint32_t atlas_h = view_height * tile_rows;
#ifdef _WIN32
	bool compose_active = compose_should_run(ldp) && (target_image != (VkImage_XDP)VK_NULL_HANDLE);
	if (compose_active) {
		// Compose needs the ck pipeline pieces too (ck_fill_rp + ck_fill_image).
		// ck_init_pipeline is idempotent and self-contained.
		if (ck_init_pipeline(ldp, (VkFormat)target_format)) {
			VkImageView composed = compose_run_pre_weave(ldp, cmd_buffer, atlas_view,
			                                              atlas_w, atlas_h,
			                                              tile_columns, tile_rows);
			if (composed != VK_NULL_HANDLE) {
				weaver_input = composed;
			}
			// On compose failure (e.g. no WGC frame yet) just pass through
			// — atlas alpha=0 regions render as black RGB through the weaver,
			// recovers next frame once WGC produces a frame.
		}
	} else
#endif
	{
		ck_active = ck_should_run(ldp) && (target_image != (VkImage_XDP)VK_NULL_HANDLE);
		if (ck_active) {
			if (ck_init_pipeline(ldp, (VkFormat)target_format)) {
				VkImageView filled = ck_run_pre_weave_fill(ldp, cmd_buffer, atlas_view,
				                                            atlas_w, atlas_h);
				if (filled != VK_NULL_HANDLE) {
					weaver_input = filled;
				} else {
					ck_active = false;
				}
			} else {
				ck_active = false;
			}
		}
	}

	// SR weaver expects SBS atlas as left_view, VK_NULL_HANDLE as right
	leiasr_weave(ldp->leiasr,
	             cmd_buffer,
	             weaver_input,
	             (VkImageView)VK_NULL_HANDLE,
	             viewport,
	             (int)view_width,
	             (int)view_height,
	             (VkFormat)view_format,
	             target_fb,
	             (int)target_width,
	             (int)target_height,
	             (VkFormat)target_format);

	// Post-weave transparency pass:
	//   - compose path: alpha-gate samples the ORIGINAL atlas to derive
	//     the screen-space "all views α==0" mask, zeroes alpha on those
	//     pixels — DWM blends the LIVE desktop (no captured-bg lag, no
	//     magenta fringe at silhouettes).
	//   - chroma-key fallback: legacy strip.
#ifdef _WIN32
	if (compose_active && target_image != (VkImage_XDP)VK_NULL_HANDLE) {
		alpha_gate_run_post_weave(ldp, cmd_buffer,
		                          (VkImage)target_image, atlas_view,
		                          target_width, target_height,
		                          tile_columns, tile_rows);
	} else
#endif
	if (ck_active) {
		ck_run_post_weave_strip(ldp, cmd_buffer, (VkImage)target_image,
		                         target_width, target_height);
	}
}

static bool
leia_dp_get_predicted_eye_positions(struct xrt_display_processor *xdp, struct xrt_eye_positions *out_eye_pos)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	// leiasr_eye_pair is #defined to xrt_eye_positions in leia_types.h
	if (!leiasr_get_predicted_eye_positions(ldp->leiasr, (struct leiasr_eye_pair *)out_eye_pos)) {
		return false;
	}
	// In 2D mode, average L/R to a single midpoint eye.
	if (ldp->view_count == 1 && out_eye_pos->count >= 2) {
		out_eye_pos->eyes[0].x = (out_eye_pos->eyes[0].x + out_eye_pos->eyes[1].x) * 0.5f;
		out_eye_pos->eyes[0].y = (out_eye_pos->eyes[0].y + out_eye_pos->eyes[1].y) * 0.5f;
		out_eye_pos->eyes[0].z = (out_eye_pos->eyes[0].z + out_eye_pos->eyes[1].z) * 0.5f;
		out_eye_pos->count = 1;
	}
	return true;
}

static bool
leia_dp_get_window_metrics(struct xrt_display_processor *xdp, struct xrt_window_metrics *out_metrics)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	// leiasr_window_metrics is #defined to xrt_window_metrics in leia_types.h
	return leiasr_get_window_metrics(ldp->leiasr, (struct leiasr_window_metrics *)out_metrics);
}

static bool
leia_dp_request_display_mode(struct xrt_display_processor *xdp, bool enable_3d)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	bool ok = leiasr_request_display_mode(ldp->leiasr, enable_3d);
	if (ok) {
		ldp->view_count = enable_3d ? 2 : 1;
	}
	return ok;
}

static bool
leia_dp_get_hardware_3d_state(struct xrt_display_processor *xdp, bool *out_is_3d)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	return leiasr_get_hardware_3d_state(ldp->leiasr, out_is_3d);
}

static bool
leia_dp_get_display_dimensions(struct xrt_display_processor *xdp, float *out_width_m, float *out_height_m)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	struct leiasr_display_dimensions dims = {};
	if (!leiasr_get_display_dimensions(ldp->leiasr, &dims) || !dims.valid) {
		return false;
	}
	*out_width_m = dims.width_m;
	*out_height_m = dims.height_m;
	return true;
}

static bool
leia_dp_get_display_pixel_info(struct xrt_display_processor *xdp,
                               uint32_t *out_pixel_width,
                               uint32_t *out_pixel_height,
                               int32_t *out_screen_left,
                               int32_t *out_screen_top)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	float w_m, h_m; // unused but required by API
	return leiasr_get_display_pixel_info(ldp->leiasr, out_pixel_width, out_pixel_height, out_screen_left,
	                                     out_screen_top, &w_m, &h_m);
}

static VkRenderPass
leia_dp_get_render_pass(struct xrt_display_processor *xdp)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	return ldp->render_pass;
}

static bool
leia_dp_is_alpha_native(struct xrt_display_processor *xdp)
{
	(void)xdp;
	// SR Vulkan weaver outputs opaque RGB; transparency requires the
	// chroma-key fill+strip trick implemented in this DP.
	return false;
}

static void
leia_dp_set_chroma_key(struct xrt_display_processor *xdp,
                       uint32_t key_color,
                       bool transparent_bg_enabled)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	// Keep ck_color/ck_enabled current — chroma-key is the fallback.
	ldp->ck_color = (key_color != 0) ? key_color : kDefaultChromaKey;
	ldp->ck_enabled = transparent_bg_enabled;

#ifdef _WIN32
	// Preferred path: WGC desktop capture + per-tile compose-under-bg.
	// On any failure (older Windows, capture blocked, env-disabled), fall
	// through to chroma-key.
	if (transparent_bg_enabled && !ldp->bg_compose_enabled && ldp->hwnd_opaque != nullptr) {
		ldp->bg_capture = leia_bg_capture_create((HWND)ldp->hwnd_opaque);
		if (ldp->bg_capture != nullptr) {
			if (compose_import_bg_image(ldp)) {
				ldp->bg_compose_enabled = true;
				ldp->ck_enabled = false;
				U_LOG_W("Leia VK DP: transparency = compose-under-bg (WGC)");
				return;
			}
			U_LOG_W("Leia VK DP: bg image import failed — falling back to chroma-key");
			leia_bg_capture_destroy(ldp->bg_capture);
			ldp->bg_capture = nullptr;
		}
	}
#endif

	if (transparent_bg_enabled) {
		U_LOG_W("Leia VK DP: transparency = chroma-key (key=0x%06x %s)",
		        ldp->ck_color & 0x00FFFFFFu,
		        (key_color != 0) ? "— app override" : "— DP default magenta");
	} else {
		U_LOG_I("Leia VK DP: chroma-key disabled");
	}
}

static void
leia_dp_destroy(struct xrt_display_processor *xdp)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	struct vk_bundle *vk = ldp->vk;

	if (vk != NULL) {
		compose_release_resources(ldp);
		ck_release_resources(ldp);
		if (ldp->render_pass != VK_NULL_HANDLE) {
			vk->vkDestroyRenderPass(vk->device, ldp->render_pass, NULL);
		}
	}

	leiasr_destroy(ldp->leiasr);
	free(ldp);
}


/*
 *
 * Factory function — matches xrt_dp_factory_vk_fn_t signature.
 *
 */

extern "C" xrt_result_t
leia_dp_factory_vk(void *vk_bundle_ptr,
                   void *vk_cmd_pool,
                   void *window_handle,
                   int32_t target_format,
                   struct xrt_display_processor **out_xdp)
{
	// Extract Vulkan handles from vk_bundle.
	struct vk_bundle *vk = (struct vk_bundle *)vk_bundle_ptr;

	struct leiasr *leiasr = NULL;
	xrt_result_t ret = leiasr_create(5.0, vk->device, vk->physical_device, vk->main_queue->queue,
	                                 (VkCommandPool)(uintptr_t)vk_cmd_pool, window_handle, &leiasr);
	if (ret != XRT_SUCCESS || leiasr == NULL) {
		U_LOG_W("Failed to create SR Vulkan weaver, continuing without interlacing");
		return ret != XRT_SUCCESS ? ret : XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct leia_display_processor *ldp = (struct leia_display_processor *)calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		leiasr_destroy(leiasr);
		return XRT_ERROR_ALLOCATION;
	}

	// Create a render pass compatible with the SR weaver's output.
	// The weaver renders to a single color attachment (no depth).
	// Use the target_format passed by the compositor, or B8G8R8A8_UNORM as default.
	VkFormat rp_format = (target_format != 0) ? (VkFormat)target_format : VK_FORMAT_B8G8R8A8_UNORM;
	VkAttachmentDescription color_attachment = {
	    .format = rp_format,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	};
	VkAttachmentReference color_ref = {
	    .attachment = 0,
	    .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkSubpassDescription subpass = {
	    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
	    .colorAttachmentCount = 1,
	    .pColorAttachments = &color_ref,
	};
	VkRenderPassCreateInfo rp_info = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &color_attachment,
	    .subpassCount = 1,
	    .pSubpasses = &subpass,
	};
	VkRenderPass render_pass = VK_NULL_HANDLE;
	VkResult vk_ret = vk->vkCreateRenderPass(vk->device, &rp_info, NULL, &render_pass);
	if (vk_ret != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: failed to create render pass: %d", vk_ret);
		leiasr_destroy(leiasr);
		free(ldp);
		return XRT_ERROR_VULKAN;
	}

	ldp->base.process_atlas = leia_dp_process_atlas;
	ldp->base.get_render_pass = leia_dp_get_render_pass;
	ldp->base.get_predicted_eye_positions = leia_dp_get_predicted_eye_positions;
	ldp->base.get_window_metrics = leia_dp_get_window_metrics;
	ldp->base.request_display_mode = leia_dp_request_display_mode;
	ldp->base.get_hardware_3d_state = leia_dp_get_hardware_3d_state;
	ldp->base.get_display_dimensions = leia_dp_get_display_dimensions;
	ldp->base.get_display_pixel_info = leia_dp_get_display_pixel_info;
	ldp->base.is_alpha_native = leia_dp_is_alpha_native;
	ldp->base.set_chroma_key = leia_dp_set_chroma_key;
	ldp->base.destroy = leia_dp_destroy;

	ldp->leiasr = leiasr;
	ldp->vk = vk;
	ldp->render_pass = render_pass;
	ldp->view_count = 2;
	ldp->hwnd_opaque = window_handle;

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR display processor (factory, owns weaver, render_pass=%p)",
	        (void *)render_pass);

	return XRT_SUCCESS;
}


/*
 *
 * Legacy creation function — wraps an existing leiasr handle.
 * Kept for backward compatibility during the refactoring transition.
 *
 */

extern "C" xrt_result_t
leia_display_processor_create(struct leiasr *leiasr, struct xrt_display_processor **out_xdp)
{
	if (leiasr == NULL || out_xdp == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct leia_display_processor *ldp = (struct leia_display_processor *)calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	ldp->base.process_atlas = leia_dp_process_atlas;
	ldp->base.get_predicted_eye_positions = leia_dp_get_predicted_eye_positions;
	ldp->base.get_window_metrics = leia_dp_get_window_metrics;
	ldp->base.request_display_mode = leia_dp_request_display_mode;
	ldp->base.get_hardware_3d_state = leia_dp_get_hardware_3d_state;
	ldp->base.get_display_dimensions = leia_dp_get_display_dimensions;
	ldp->base.get_display_pixel_info = leia_dp_get_display_pixel_info;
	ldp->base.is_alpha_native = leia_dp_is_alpha_native;
	ldp->base.set_chroma_key = leia_dp_set_chroma_key;
	// Legacy: does NOT own leiasr — use a destroy that skips leiasr_destroy.
	// For now just assign the full destroy; callers will be migrated to factory.
	ldp->base.destroy = leia_dp_destroy;

	ldp->leiasr = leiasr;
	ldp->view_count = 2;

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR display processor (legacy, owns weaver)");

	return XRT_SUCCESS;
}
