// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simulation display processor: SBS, anaglyph, alpha-blend output.
 *
 * Implements side-by-side, anaglyph, and alpha-blend stereo output
 * modes for development and testing on regular 2D displays.
 *
 * SBS mode is a no-op (compositor viewport config handles layout).
 * Anaglyph and blend modes use fullscreen-triangle fragment shaders.
 *
 * @author David Fattal
 * @ingroup drv_sim_display
 */

#include "sim_display_interface.h"
#include "sim_display_zone_common.h"

#include "xrt/xrt_display_processor.h"
#include "xrt/xrt_display_metrics.h"

#include "vk/vk_helpers.h"
#include "util/u_debug.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <stdlib.h>
#include <string.h>

DEBUG_GET_ONCE_FLOAT_OPTION(sim_display_nominal_z_m, "SIM_DISPLAY_NOMINAL_Z_M", 0.60f)

/*!
 * Audit the weave target against the declared panel (#817).
 *
 * sim_display writes its own pixels with a fullscreen triangle, so it renders
 * correctly at ANY target size and at any position — none of its output modes
 * (SBS/anaglyph/blend/squeezed/quad/passthrough) is column-interlaced, so none
 * is sensitive to the woven texture's exact physical pixel size or to where the
 * target lands on the panel. A real lenticular weaver is sensitive to both: the
 * interlacing phase is a function of the target's absolute physical-pixel
 * origin, and any resample between the woven texture and scanout destroys the
 * pattern outright.
 *
 * That makes a green sim_display run silent on precisely the properties that
 * decide whether real weaving will work. This option makes sim_display *check*
 * them instead: it compares the weave target against the panel dimensions the
 * driver declared (SIM_DISPLAY_PIXEL_W/H) and reports the canvas rect the
 * compositor asked for, so a mis-sized or displaced target is visible with no
 * vendor hardware present. Off by default — purely diagnostic, never changes
 * what is rendered.
 */
DEBUG_GET_ONCE_BOOL_OPTION(sim_display_strict_panel, "SIM_DISPLAY_STRICT_PANEL", false)

/*!
 * #817 — stripe width, in panel pixels, for SIM_DISPLAY_OUTPUT=interlaced.
 *
 * 1 is the default because 1 is what a lenticular actually needs: at a
 * one-pixel period any resample between the woven texture and scanout
 * destroys the pattern, which is the whole point of the mode. A larger value
 * widens the stripes so a human can see the pattern (and the phase shift when
 * the window moves) at a glance, at the cost of some of that sensitivity.
 */
DEBUG_GET_ONCE_NUM_OPTION(sim_display_interlace_period, "SIM_DISPLAY_INTERLACE_PERIOD", 1)

// SPIR-V shader headers (generated at build time by spirv_shaders())
#include "sim_display/shaders/fullscreen.vert.h"
#include "sim_display/shaders/anaglyph.frag.h"
#include "sim_display/shaders/blend.frag.h"
#include "sim_display/shaders/sbs.frag.h"
#include "sim_display/shaders/squeezed_sbs.frag.h"
#include "sim_display/shaders/quad.frag.h"
#include "sim_display/shaders/passthrough.frag.h"
#include "sim_display/shaders/interlaced.frag.h"


/*!
 * Implementation struct for the simulation display processor.
 */
struct sim_display_processor
{
	struct xrt_display_processor base;
	struct vk_bundle *vk;
	VkRenderPass render_pass;
	//! One per output mode (SBS, anaglyph, blend, squeezed SBS, quad,
	//! passthrough, interlaced).
	VkPipeline pipelines[SIM_DP_PIPELINE_COUNT];
	//! #817: SIM_DISPLAY_INTERLACE_PERIOD, latched at creation.
	int32_t interlace_period_px;
	VkPipelineLayout pipeline_layout;
	VkDescriptorSetLayout desc_layout;
	VkDescriptorPool desc_pool;
	VkDescriptorSet desc_set;       //!< Persistent descriptor set (allocated once)
	VkSampler sampler;

	//! Nominal viewer parameters for faked eye positions.
	float ipd_m;
	float nominal_x_m;
	float nominal_y_m;
	float nominal_z_m;

	//! When true, the app requested a transparent background — clear the
	//! weave target to alpha=0 so alpha<1 regions stay see-through.
	bool transparent_bg;

	//! #491 part 3 — the runtime's flattened 2D-under backdrop for the next
	//! process_atlas (set via set_background_2d). sim_display is a test double:
	//! it records the handoff (proving the runtime wiring) but does not capture
	//! a desktop, so it has no `backdrop over desktop` to weave under (the real
	//! visual proof is the Leia DP). Stored + logged so the slot is a non-NULL
	//! in-repo consumer exercisable headlessly. VK_NULL_HANDLE ⟹ no backdrop.
	VkImageView bg_view;
	uint32_t bg_w, bg_h;

	//! #224 / ADR-027 local-zone test double (Vulkan port of the D3D11
	//! triple). Shared env config + change-gated publish/clear logging.
	struct sim_zone_config zone_cfg;
	int32_t zone_last_x, zone_last_y;
	uint32_t zone_last_w, zone_last_h;
	uint32_t zone_last_mask_w, zone_last_mask_h;
	uint64_t zone_last_seq;
	bool zone_active; //!< A client mask is currently published (not cleared).

	//! #1484 / ADR-021 — the atlas encoding the runtime last declared for the
	//! next process_atlas (set via base slot 14 `set_atlas_encoding`).
	//! calloc-zeroed ⟹ XRT_ATLAS_ENCODING_ENCODED (Model A), which is also
	//! what an undeclaring runtime means, so the default is honest.
	enum xrt_atlas_encoding atlas_encoding;
	bool atlas_encoding_declared; //!< Distinguishes "declared ENCODED" from "never declared".

	//! #817 — last geometry reported by the SIM_DISPLAY_STRICT_PANEL audit.
	//! Change-gated so a steady state logs once, never per frame.
	bool geom_reported;
	uint32_t geom_last_target_w, geom_last_target_h;
	int32_t geom_last_canvas_x, geom_last_canvas_y;
	uint32_t geom_last_canvas_w, geom_last_canvas_h;
};

static inline struct sim_display_processor *
sim_display_processor(struct xrt_display_processor *xdp)
{
	return (struct sim_display_processor *)xdp;
}


/*
 *
 * Anaglyph/blend output: fullscreen triangle with fragment shader.
 *
 */

/*!
 * Push constant data for tile layout parameters.
 */
struct tile_push_constants
{
	float inv_tile_columns;
	float inv_tile_rows;
	float tile_columns;
	float tile_rows;
	//! #817: interlace phase (canvas_offset_x) and stripe width, in panel
	//! pixels. Only interlaced.frag declares these; the other fragment
	//! shaders declare just the first four floats, which is legal — a
	//! smaller push block reading the head of a larger range.
	float phase_px;
	float period_px;
	float pad0;
	float pad1;
};

static void
sim_dp_process_atlas(struct xrt_display_processor *xdp,
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
	// phase correction once Leia SR SDK supports sub-rect offset. For every
	// sim_display mode BUT interlaced the canvas rect does not affect what is
	// rendered (a fullscreen triangle is correct at any size or offset) — it
	// is otherwise consumed only by the SIM_DISPLAY_STRICT_PANEL audit, which
	// reports the geometry a real lenticular weaver would be sensitive to.
	// SIM_DISPLAY_OUTPUT=interlaced (#817) does consume canvas_offset_x: it is
	// the interlace phase, so a displaced target visibly flips which eye lands
	// on the even columns.

	(void)view_format; // colorspace handled by SRV/format selection, not this arg

	(void)atlas_image; // sim_display uses atlas_view via shader sampling
	(void)target_image; // sim_display uses target_fb via render pass
	struct sim_display_processor *sdp = sim_display_processor(xdp);

	// #817: report the weave geometry against the declared panel. Diagnostic
	// only — it never changes what is rendered.
	if (debug_get_bool_option_sim_display_strict_panel()) {
		const bool changed = !sdp->geom_reported ||                        //
		                     sdp->geom_last_target_w != target_width ||    //
		                     sdp->geom_last_target_h != target_height ||   //
		                     sdp->geom_last_canvas_x != canvas_offset_x || //
		                     sdp->geom_last_canvas_y != canvas_offset_y || //
		                     sdp->geom_last_canvas_w != canvas_width ||    //
		                     sdp->geom_last_canvas_h != canvas_height;

		if (changed) {
			float panel_w_m = 0.0f, panel_h_m = 0.0f;
			uint32_t panel_px_w = 0, panel_px_h = 0;
			sim_display_get_panel_metrics(&panel_w_m, &panel_h_m, &panel_px_w, &panel_px_h);

			const bool fills_panel = panel_px_w == target_width && panel_px_h == target_height;
			const bool at_origin = canvas_offset_x == 0 && canvas_offset_y == 0;

			U_LOG_W("sim_display STRICT PANEL (#817): weave target %ux%u, panel %ux%u (%.3fx%.3f m) — %s",
			        target_width, target_height, panel_px_w, panel_px_h, (double)panel_w_m,
			        (double)panel_h_m,
			        fills_panel ? "target is EXACTLY panel-sized (a real weaver gets 1:1 pixels)"
			                    : "target is NOT panel-sized — windowed, or the desktop is SCALED; a "
			                      "real weaver would be resampled and lose the interlace pattern");
			// #817: only the interlaced output consumes the offset. Every
			// other mode renders the same pixels wherever the target sits,
			// so a phase error genuinely cannot show up in what it draws.
			const bool phase_visible = sim_display_get_output_mode() == SIM_DISPLAY_OUTPUT_INTERLACED;
			U_LOG_W("sim_display STRICT PANEL (#817): canvas offset (%d,%d) size %ux%u%s; atlas view "
			        "%ux%u grid %ux%u. %s",
			        canvas_offset_x, canvas_offset_y, canvas_width, canvas_height,
			        at_origin ? " (panel origin)" : " (displaced — phase-critical for a real weaver)",
			        view_width, view_height, tile_columns, tile_rows,
			        phase_visible ? "SIM_DISPLAY_OUTPUT=interlaced USES the offset as the "
			                        "interlace phase, so a phase error is visible in the output."
			                      : "This mode ignores the offset (sim_display has no "
			                        "set_present_origin slot), so phase errors are INVISIBLE in "
			                        "what it draws — run SIM_DISPLAY_OUTPUT=interlaced to see them.");

			sdp->geom_reported = true;
			sdp->geom_last_target_w = target_width;
			sdp->geom_last_target_h = target_height;
			sdp->geom_last_canvas_x = canvas_offset_x;
			sdp->geom_last_canvas_y = canvas_offset_y;
			sdp->geom_last_canvas_w = canvas_width;
			sdp->geom_last_canvas_h = canvas_height;
		}
	}
	struct vk_bundle *vk = sdp->vk;

	// Read the current mode (may change at runtime via 1/2/3 keys).
	// Single-view input forces passthrough — 3D shaders need ≥2 views.
	enum sim_display_output_mode mode = sim_display_get_output_mode();
	if (tile_columns * tile_rows <= 1) {
		mode = SIM_DISPLAY_OUTPUT_PASSTHROUGH;
	}
	VkPipeline active_pipeline = sdp->pipelines[mode];
	if (sim_zone_substitute_position_preserving(mode, sdp->zone_active, "VK")) {
		active_pipeline = sdp->pipelines[SIM_DISPLAY_OUTPUT_ANAGLYPH];
	}

	if (vk == NULL || active_pipeline == VK_NULL_HANDLE) {
		return;
	}

	// Update persistent descriptor set with atlas image view
	VkDescriptorImageInfo image_info = {
	    .sampler = sdp->sampler,
	    .imageView = atlas_view,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkWriteDescriptorSet write = {
	    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	    .dstSet = sdp->desc_set,
	    .dstBinding = 0,
	    .descriptorCount = 1,
	    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .pImageInfo = &image_info,
	};

	vk->vkUpdateDescriptorSets(vk->device, 1, &write, 0, NULL);

	// Begin render pass. Clear to alpha=0 when a transparent background was
	// requested so any region the weave doesn't fully overwrite stays
	// see-through instead of opaque black (issue #392).
	VkClearValue clear_value = {.color = {{0.0f, 0.0f, 0.0f, sdp->transparent_bg ? 0.0f : 1.0f}}};
	VkRenderPassBeginInfo rp_begin = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = sdp->render_pass,
	    .framebuffer = target_fb,
	    .renderArea =
	        {
	            .offset = {0, 0},
	            .extent = {target_width, target_height},
	        },
	    .clearValueCount = 1,
	    .pClearValues = &clear_value,
	};

	vk->vkCmdBeginRenderPass(cmd_buffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

	// Bind pipeline and descriptor set
	vk->vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, active_pipeline);
	vk->vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sdp->pipeline_layout, 0, 1,
	                             &sdp->desc_set, 0, NULL);

	// Push tile layout constants to fragment shader
	struct tile_push_constants pc = {
	    .inv_tile_columns = 1.0f / (float)tile_columns,
	    .inv_tile_rows = 1.0f / (float)tile_rows,
	    .tile_columns = (float)tile_columns,
	    .tile_rows = (float)tile_rows,
	    // #817: the interlace phase is the target's panel-relative X origin.
	    .phase_px = (float)canvas_offset_x,
	    .period_px = (float)sdp->interlace_period_px,
	};
	vk->vkCmdPushConstants(cmd_buffer, sdp->pipeline_layout,
	                        VK_SHADER_STAGE_FRAGMENT_BIT, 0,
	                        sizeof(pc), &pc);

	// Set dynamic viewport and scissor
	VkViewport viewport = {
	    .x = 0.0f,
	    .y = 0.0f,
	    .width = (float)target_width,
	    .height = (float)target_height,
	    .minDepth = 0.0f,
	    .maxDepth = 1.0f,
	};
	vk->vkCmdSetViewport(cmd_buffer, 0, 1, &viewport);

	VkRect2D scissor = {
	    .offset = {0, 0},
	    .extent = {target_width, target_height},
	};
	vk->vkCmdSetScissor(cmd_buffer, 0, 1, &scissor);

	// Draw fullscreen triangle (3 vertices, no VBO)
	vk->vkCmdDraw(cmd_buffer, 3, 1, 0, 0);

	vk->vkCmdEndRenderPass(cmd_buffer);
}


/*
 *
 * Create Vulkan pipeline resources for anaglyph/blend modes.
 *
 */

static VkResult
create_shader_module(struct vk_bundle *vk, const uint32_t *code, size_t code_size, VkShaderModule *out_module)
{
	VkShaderModuleCreateInfo create_info = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = code_size,
	    .pCode = code,
	};

	return vk->vkCreateShaderModule(vk->device, &create_info, NULL, out_module);
}

static bool
create_pipeline_resources(struct sim_display_processor *sdp, int32_t target_format)
{
	struct vk_bundle *vk = sdp->vk;
	VkResult ret;

	// 1. Create render pass (single color attachment)
	//    initialLayout = UNDEFINED: image may be in any layout (PRESENT_SRC after present, etc.)
	//    finalLayout = PRESENT_SRC_KHR: image goes directly to presentation after display processing
	//    Using LOAD_OP_CLEAR to ensure entire framebuffer is initialized (diagnostic: magenta background)
	VkAttachmentDescription color_attachment = {
	    .format = (VkFormat)target_format,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
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

	ret = vk->vkCreateRenderPass(vk->device, &rp_info, NULL, &sdp->render_pass);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create render pass: %d", ret);
		return false;
	}

	// 2. Create descriptor set layout (1 combined image sampler for atlas)
	VkDescriptorSetLayoutBinding binding = {
	    .binding = 0,
	    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .descriptorCount = 1,
	    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};

	VkDescriptorSetLayoutCreateInfo desc_layout_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = 1,
	    .pBindings = &binding,
	};

	ret = vk->vkCreateDescriptorSetLayout(vk->device, &desc_layout_info, NULL, &sdp->desc_layout);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create descriptor set layout: %d", ret);
		return false;
	}

	// 3. Create pipeline layout (with push constants for tile params)
	VkPushConstantRange push_range = {
	    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	    .offset = 0,
	    .size = sizeof(struct tile_push_constants),
	};

	VkPipelineLayoutCreateInfo pipe_layout_info = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount = 1,
	    .pSetLayouts = &sdp->desc_layout,
	    .pushConstantRangeCount = 1,
	    .pPushConstantRanges = &push_range,
	};

	ret = vk->vkCreatePipelineLayout(vk->device, &pipe_layout_info, NULL, &sdp->pipeline_layout);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create pipeline layout: %d", ret);
		return false;
	}

	// 4. Create vertex shader module (shared by all modes)
	VkShaderModule vert_module = VK_NULL_HANDLE;
	ret = create_shader_module(vk, sim_display_shaders_fullscreen_vert, sizeof(sim_display_shaders_fullscreen_vert),
	                           &vert_module);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create vertex shader module: %d", ret);
		return false;
	}

	// 5. Create one graphics pipeline per output mode (SBS, anaglyph, blend)
	//    so runtime switching is instant (no pipeline recreation).
	//    All shader modules are created upfront and all pipelines are created
	//    in a single batch call to avoid potential driver issues with module
	//    handle reuse between iterations (observed on MoltenVK/macOS).
	struct {
		const uint32_t *code;
		size_t size;
		const char *name;
	} frag_shaders[SIM_DP_PIPELINE_COUNT] = {
	    {sim_display_shaders_sbs_frag, sizeof(sim_display_shaders_sbs_frag), "SBS"},
	    {sim_display_shaders_anaglyph_frag, sizeof(sim_display_shaders_anaglyph_frag), "Anaglyph"},
	    {sim_display_shaders_blend_frag, sizeof(sim_display_shaders_blend_frag), "Blend"},
	    {sim_display_shaders_squeezed_sbs_frag, sizeof(sim_display_shaders_squeezed_sbs_frag), "Squeezed SBS"},
	    {sim_display_shaders_quad_frag, sizeof(sim_display_shaders_quad_frag), "Quad"},
	    {sim_display_shaders_passthrough_frag, sizeof(sim_display_shaders_passthrough_frag), "Passthrough"},
	    {sim_display_shaders_interlaced_frag, sizeof(sim_display_shaders_interlaced_frag), "Interlaced"},
		};

	// Create all fragment shader modules upfront (keep alive until all pipelines are created)
	VkShaderModule frag_modules[SIM_DP_PIPELINE_COUNT] = {VK_NULL_HANDLE};
	for (int i = 0; i < SIM_DP_PIPELINE_COUNT; i++) {
		ret = create_shader_module(vk, frag_shaders[i].code, frag_shaders[i].size, &frag_modules[i]);
		if (ret != VK_SUCCESS) {
			U_LOG_E("sim_display: Failed to create %s fragment shader: %d", frag_shaders[i].name, ret);
			for (int j = 0; j < i; j++)
				vk->vkDestroyShaderModule(vk->device, frag_modules[j], NULL);
			vk->vkDestroyShaderModule(vk->device, vert_module, NULL);
			return false;
		}
	}

	VkPipelineVertexInputStateCreateInfo vertex_input = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};

	VkPipelineInputAssemblyStateCreateInfo input_assembly = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};

	VkPipelineViewportStateCreateInfo viewport_state = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	    .viewportCount = 1,
	    .scissorCount = 1,
	};

	VkPipelineRasterizationStateCreateInfo rasterization = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	    .polygonMode = VK_POLYGON_MODE_FILL,
	    .cullMode = VK_CULL_MODE_NONE,
	    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	    .lineWidth = 1.0f,
	};

	VkPipelineMultisampleStateCreateInfo multisample = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	VkPipelineColorBlendAttachmentState blend_attachment = {
	    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
	                      VK_COLOR_COMPONENT_A_BIT,
	};

	VkPipelineColorBlendStateCreateInfo color_blend = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &blend_attachment,
	};

	VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic_state = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	    .dynamicStateCount = 2,
	    .pDynamicStates = dynamic_states,
	};

	// Build all pipeline create infos with their own stage arrays
	VkPipelineShaderStageCreateInfo all_stages[SIM_DP_PIPELINE_COUNT][2];
	VkGraphicsPipelineCreateInfo pipeline_infos[SIM_DP_PIPELINE_COUNT];
	for (int i = 0; i < SIM_DP_PIPELINE_COUNT; i++) {
		all_stages[i][0] = (VkPipelineShaderStageCreateInfo){
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		    .stage = VK_SHADER_STAGE_VERTEX_BIT,
		    .module = vert_module,
		    .pName = "main",
		};
		all_stages[i][1] = (VkPipelineShaderStageCreateInfo){
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		    .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		    .module = frag_modules[i],
		    .pName = "main",
		};
		pipeline_infos[i] = (VkGraphicsPipelineCreateInfo){
		    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		    .stageCount = 2,
		    .pStages = all_stages[i],
		    .pVertexInputState = &vertex_input,
		    .pInputAssemblyState = &input_assembly,
		    .pViewportState = &viewport_state,
		    .pRasterizationState = &rasterization,
		    .pMultisampleState = &multisample,
		    .pColorBlendState = &color_blend,
		    .pDynamicState = &dynamic_state,
		    .layout = sdp->pipeline_layout,
		    .renderPass = sdp->render_pass,
		    .subpass = 0,
		};
	}

	// Create all pipelines in a single batch call
	ret = vk->vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, SIM_DP_PIPELINE_COUNT, pipeline_infos, NULL,
	                                    sdp->pipelines);

	// Destroy all shader modules now that pipelines are created
	for (int i = 0; i < SIM_DP_PIPELINE_COUNT; i++)
		vk->vkDestroyShaderModule(vk->device, frag_modules[i], NULL);
	vk->vkDestroyShaderModule(vk->device, vert_module, NULL);

	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create pipelines: %d", ret);
		return false;
	}


	// 6. Create sampler (linear filtering, clamp to edge)
	VkSamplerCreateInfo sampler_info = {
	    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	    .magFilter = VK_FILTER_LINEAR,
	    .minFilter = VK_FILTER_LINEAR,
	    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
	    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .maxLod = 1.0f,
	};

	ret = vk->vkCreateSampler(vk->device, &sampler_info, NULL, &sdp->sampler);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create sampler: %d", ret);
		return false;
	}

	// 7. Create descriptor pool (persistent set, never freed individually)
	VkDescriptorPoolSize pool_size = {
	    .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .descriptorCount = 1, // One set with 1 atlas sampler
	};

	VkDescriptorPoolCreateInfo pool_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	    .maxSets = 1,
	    .poolSizeCount = 1,
	    .pPoolSizes = &pool_size,
	};

	ret = vk->vkCreateDescriptorPool(vk->device, &pool_info, NULL, &sdp->desc_pool);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create descriptor pool: %d", ret);
		return false;
	}

	// 8. Allocate persistent descriptor set (updated each frame, never freed)
	VkDescriptorSetAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	    .descriptorPool = sdp->desc_pool,
	    .descriptorSetCount = 1,
	    .pSetLayouts = &sdp->desc_layout,
	};

	ret = vk->vkAllocateDescriptorSets(vk->device, &alloc_info, &sdp->desc_set);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to allocate descriptor set: %d", ret);
		return false;
	}

	return true;
}


// #856: panel geometry -> compositor computes window-scoped Kooima.
SIM_ZONE_DEFINE_PANEL_METRIC_FNS(sim_dp, xrt_display_processor)

static bool
sim_dp_get_predicted_eye_positions(struct xrt_display_processor *xdp, struct xrt_eye_positions *out)
{
	struct sim_display_processor *sdp = sim_display_processor(xdp);
	float half_ipd = sdp->ipd_m / 2.0f;
	uint32_t vc = sim_display_get_view_count();

	if (vc == 1) {
		out->eyes[0] = (struct xrt_eye_position){sdp->nominal_x_m, sdp->nominal_y_m, sdp->nominal_z_m};
		out->count = 1;
	} else if (vc >= 4) {
		out->eyes[0] = (struct xrt_eye_position){sdp->nominal_x_m - half_ipd, sdp->nominal_y_m - 0.032f, sdp->nominal_z_m};
		out->eyes[1] = (struct xrt_eye_position){sdp->nominal_x_m + half_ipd, sdp->nominal_y_m - 0.032f, sdp->nominal_z_m};
		out->eyes[2] = (struct xrt_eye_position){sdp->nominal_x_m - half_ipd, sdp->nominal_y_m + 0.032f, sdp->nominal_z_m};
		out->eyes[3] = (struct xrt_eye_position){sdp->nominal_x_m + half_ipd, sdp->nominal_y_m + 0.032f, sdp->nominal_z_m};
		out->count = 4;
	} else {
		out->eyes[0] = (struct xrt_eye_position){sdp->nominal_x_m - half_ipd, sdp->nominal_y_m, sdp->nominal_z_m};
		out->eyes[1] = (struct xrt_eye_position){sdp->nominal_x_m + half_ipd, sdp->nominal_y_m, sdp->nominal_z_m};
		out->count = 2;
	}
	out->timestamp_ns = os_monotonic_get_ns();
	out->valid = true;
	out->is_tracking = sim_display_fake_tracking_is_tracking(); // false unless SIM_DISPLAY_FAKE_TRACKING (#441)
	return true;
}

static VkRenderPass
sim_dp_get_render_pass(struct xrt_display_processor *xdp)
{
	struct sim_display_processor *sdp = sim_display_processor(xdp);
	return sdp->render_pass;
}

static void
sim_dp_destroy(struct xrt_display_processor *xdp)
{
	struct sim_display_processor *sdp = sim_display_processor(xdp);

	if (sdp->vk != NULL) {
		struct vk_bundle *vk = sdp->vk;

		if (sdp->desc_pool != VK_NULL_HANDLE) {
			vk->vkDestroyDescriptorPool(vk->device, sdp->desc_pool, NULL);
		}
		if (sdp->sampler != VK_NULL_HANDLE) {
			vk->vkDestroySampler(vk->device, sdp->sampler, NULL);
		}
		for (int i = 0; i < SIM_DP_PIPELINE_COUNT; i++) {
			if (sdp->pipelines[i] != VK_NULL_HANDLE) {
				vk->vkDestroyPipeline(vk->device, sdp->pipelines[i], NULL);
			}
		}
		if (sdp->pipeline_layout != VK_NULL_HANDLE) {
			vk->vkDestroyPipelineLayout(vk->device, sdp->pipeline_layout, NULL);
		}
		if (sdp->desc_layout != VK_NULL_HANDLE) {
			vk->vkDestroyDescriptorSetLayout(vk->device, sdp->desc_layout, NULL);
		}
		if (sdp->render_pass != VK_NULL_HANDLE) {
			vk->vkDestroyRenderPass(vk->device, sdp->render_pass, NULL);
		}
	}

	free(sdp);
}


/*
 * sim_display passes per-pixel alpha through its output stage to the
 * framebuffer (anaglyph / SBS / blend shaders sample atlas alpha and write
 * it to the target). No chroma-key trick needed — declare alpha-native via
 * the vtable so callers can route transparency requests directly.
 */
static bool
sim_dp_is_alpha_native(struct xrt_display_processor *xdp)
{
	(void)xdp;
	return true;
}

/*
 * #1484 / ADR-021 — the in-repo, hardware-free proof that the VK compositor's
 * per-frame atlas-encoding declaration reaches a DP at all.
 *
 * Unlike the D3D11 twin this VK DP has NO conversion knob: its pipelines sample
 * the atlas and write the result through, so it cannot honour a LINEAR atlas by
 * encoding on output. It therefore records the declaration and logs it rather
 * than acting on it — which is exactly what makes it a usable test double for
 * the runtime half, while a real weaver (Leia Linux) consumes the same slot to
 * drive `srWeaverSetShaderSRGBConversion`.
 *
 * Logged on CHANGE only — process_atlas is a per-frame path and the runtime
 * asserts this before every one of them.
 */
static void
sim_dp_set_atlas_encoding(struct xrt_display_processor *xdp, enum xrt_atlas_encoding atlas_encoding)
{
	struct sim_display_processor *sdp = sim_display_processor(xdp);
	if (sdp->atlas_encoding_declared && sdp->atlas_encoding == atlas_encoding) {
		return;
	}
	U_LOG_W("SIM DP (VK) #1484: runtime declared atlas encoding %s (was %s)",
	        atlas_encoding == XRT_ATLAS_ENCODING_LINEAR ? "LINEAR" : "ENCODED",
	        !sdp->atlas_encoding_declared                      ? "undeclared"
	        : sdp->atlas_encoding == XRT_ATLAS_ENCODING_LINEAR ? "LINEAR"
	                                                           : "ENCODED");
	sdp->atlas_encoding = atlas_encoding;
	sdp->atlas_encoding_declared = true;
}

/*
 * ADR-021 §3 — the handoff encoding this DP actually accepts. ENCODED only: see
 * set_atlas_encoding above, this DP has no output-encode stage. Filling the
 * slot is documentation rather than behaviour (NULL already means ENCODED, and
 * no VK-side caller queries it today — only the D3D11 service does), but an
 * explicit ENCODED is what stops the next reader assuming the VK twin matches
 * the D3D11 one, which declares EITHER because it really does encode.
 */
static enum xrt_dp_color_capability
sim_dp_get_handoff_color_capability(struct xrt_display_processor *xdp)
{
	(void)xdp;
	return XRT_DP_COLOR_ENCODED;
}

// #491 part 3 — store the runtime's flattened 2D-under backdrop for the next
// process_atlas. sim_display is a test double (no captured desktop), so it
// records the handoff rather than compositing pixels; the one-shot WARN proves
// the runtime → DP set_background_2d wiring works without Leia hardware.
static void
sim_dp_set_background_2d(struct xrt_display_processor *xdp,
                         VkImageView background_view,
                         uint32_t width,
                         uint32_t height)
{
	struct sim_display_processor *sdp = sim_display_processor(xdp);
	sdp->bg_view = background_view;
	sdp->bg_w = width;
	sdp->bg_h = height;
	if (background_view != VK_NULL_HANDLE) {
		static bool logged = false;
		if (!logged) {
			logged = true;
			U_LOG_W("sim_display #491 part3: received 2D-under backdrop %ux%u via set_background_2d "
			        "(test double — handoff recorded, not composited)",
			        width, height);
		}
	}
}


/*
 *
 * #224 / ADR-027 local 2D/3D zones — Vulkan port of the D3D11 test double.
 *
 * Change-gated log lines proving the runtime publishes the right wish at the
 * right screen anchor (CI-greppable), without faking panel behavior.
 * SIM_DISPLAY_ZONE_DUMP is LOG-ONLY on this variant: a CPU readback of the
 * published VkImageView would need a oneshot command buffer + staging buffer
 * the VK test double doesn't carry (it only records into the compositor's
 * cmd buffer) — the D3D11 and GL variants provide the real content dumps.
 *
 */

static bool
sim_dp_get_local_zone_caps(struct xrt_display_processor *xdp, struct xrt_dp_local_zone_caps *out_caps)
{
	struct sim_display_processor *sdp = sim_display_processor(xdp);
	if (out_caps == NULL || out_caps->struct_size < XRT_DP_LOCAL_ZONE_CAPS_SIZE_V1) {
		// V1 floor only — see the D3D11 variant for the append rationale.
		return false;
	}
	SIM_ZONE_FILL_CAPS(out_caps, &sdp->zone_cfg);
	return true;
}

static bool
sim_dp_publish_local_zone_mask(struct xrt_display_processor *xdp,
                               VkImageView mask_view,
                               uint32_t mask_width,
                               uint32_t mask_height,
                               int32_t screen_x,
                               int32_t screen_y,
                               uint32_t screen_w,
                               uint32_t screen_h,
                               uint64_t seq)
{
	struct sim_display_processor *sdp = sim_display_processor(xdp);
	if (mask_view == (VkImageView)0 || mask_width == 0 || mask_height == 0) {
		return false;
	}

	// Change-gated geometry log (seq ticks every frame — not a change).
	bool geo_changed = !sdp->zone_active || screen_x != sdp->zone_last_x || screen_y != sdp->zone_last_y ||
	                   screen_w != sdp->zone_last_w || screen_h != sdp->zone_last_h ||
	                   mask_width != sdp->zone_last_mask_w || mask_height != sdp->zone_last_mask_h;
	if (geo_changed) {
		U_LOG_W("SIM ZONE PUBLISH (VK): mask=%ux%u screen=(%d,%d %ux%u) grid=%ux%u seq=%llu", mask_width,
		        mask_height, screen_x, screen_y, screen_w, screen_h, sdp->zone_cfg.grid_w, sdp->zone_cfg.grid_h,
		        (unsigned long long)seq);
		if (sdp->zone_cfg.dump) {
			static bool dump_noted = false;
			if (!dump_noted) {
				dump_noted = true;
				U_LOG_W("SIM ZONE DUMP (VK): log-only on this variant (no oneshot readback path) — "
				        "use the D3D11 or GL variant for content dumps");
			}
		}
	}
	sdp->zone_active = true;
	sdp->zone_last_x = screen_x;
	sdp->zone_last_y = screen_y;
	sdp->zone_last_w = screen_w;
	sdp->zone_last_h = screen_h;
	sdp->zone_last_mask_w = mask_width;
	sdp->zone_last_mask_h = mask_height;
	sdp->zone_last_seq = seq;
	return true;
}

static bool
sim_dp_clear_local_zone_mask(struct xrt_display_processor *xdp)
{
	struct sim_display_processor *sdp = sim_display_processor(xdp);
	if (sdp->zone_active) {
		U_LOG_W("SIM ZONE CLEAR (VK): client contribution withdrawn (last seq=%llu)",
		        (unsigned long long)sdp->zone_last_seq);
	}
	sdp->zone_active = false;
	return true;
}


/*
 *
 * Exported creation function.
 *
 */

xrt_result_t
sim_display_processor_create(enum sim_display_output_mode mode,
                             struct vk_bundle *vk,
                             int32_t target_format,
                             struct xrt_display_processor **out_xdp)
{
	if (out_xdp == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct sim_display_processor *sdp = calloc(1, sizeof(*sdp));
	if (sdp == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	// ADR-020 rule 1: advertise the vtable size so the runtime knows which
	// slots this plug-in actually built (calloc already zeroed reserved_0).
	sdp->base.struct_size = (uint32_t)sizeof(struct xrt_display_processor);
	sdp->base.destroy = sim_dp_destroy;
	sdp->base.get_render_pass = sim_dp_get_render_pass;
	sdp->base.get_predicted_eye_positions = sim_dp_get_predicted_eye_positions;
	sdp->base.get_display_dimensions = sim_dp_get_display_dimensions;   // #856
	sdp->base.get_display_pixel_info = sim_dp_get_display_pixel_info;   // #856
	sdp->base.is_alpha_native = sim_dp_is_alpha_native;
	sdp->base.set_background_2d = sim_dp_set_background_2d; // #491 part 3
	sdp->base.get_handoff_color_capability = sim_dp_get_handoff_color_capability; // #1484 / ADR-021
	sdp->base.set_atlas_encoding = sim_dp_set_atlas_encoding;                     // #1484 / ADR-021
	sdp->base.get_local_zone_caps = sim_dp_get_local_zone_caps;          // #224 / ADR-027
	sdp->base.publish_local_zone_mask = sim_dp_publish_local_zone_mask;  // #224 / ADR-027
	sdp->base.clear_local_zone_mask = sim_dp_clear_local_zone_mask;      // #224 / ADR-027

	// #224 / ADR-027 zone test double config (shared parser).
	sim_zone_config_from_env(&sdp->zone_cfg, "VK");

	// Nominal viewer parameters (same defaults as sim_display_hmd_create)
	sdp->ipd_m = 0.06f;
	sdp->nominal_x_m = 0.0f;
	sdp->nominal_y_m = 0.1f;
	sdp->nominal_z_m = debug_get_float_option_sim_display_nominal_z_m();

	// #817: interlace stripe width; clamped to >= 1 so a bad value can never
	// divide by zero in the shader.
	sdp->interlace_period_px = (int32_t)debug_get_num_option_sim_display_interlace_period();
	if (sdp->interlace_period_px < 1) {
		sdp->interlace_period_px = 1;
	}

	if (vk == NULL) {
		U_LOG_E("sim_display: Vulkan bundle required for display processor");
		free(sdp);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	sdp->vk = vk;
	sdp->base.process_atlas = sim_dp_process_atlas;

	if (!create_pipeline_resources(sdp, target_format)) {
		U_LOG_E("sim_display: Failed to create pipeline resources");
		sim_dp_destroy(&sdp->base);
		return XRT_ERROR_VULKAN;
	}

	// Set the initial output mode (atomic global read by process_atlas each frame)
	sim_display_set_output_mode(mode);

	U_LOG_W("Created sim display processor (all %d pipelines), initial mode: %s", SIM_DP_PIPELINE_COUNT,
	        mode == SIM_DISPLAY_OUTPUT_SBS           ? "SBS" :
	        mode == SIM_DISPLAY_OUTPUT_ANAGLYPH       ? "Anaglyph" :
	        mode == SIM_DISPLAY_OUTPUT_SQUEEZED_SBS   ? "Squeezed SBS" :
	        mode == SIM_DISPLAY_OUTPUT_QUAD            ? "Quad" :
	        mode == SIM_DISPLAY_OUTPUT_INTERLACED      ? "Interlaced" :
	        mode == SIM_DISPLAY_OUTPUT_PASSTHROUGH     ? "Passthrough" : "Blend");

	*out_xdp = &sdp->base;
	return XRT_SUCCESS;
}


/*
 *
 * Factory function — matches xrt_dp_factory_vk_fn_t signature.
 *
 */

xrt_result_t
sim_display_dp_factory_vk(void *vk_bundle_ptr,
                          void *vk_cmd_pool,
                          void *window_handle,
                          int32_t target_format,
                          struct xrt_display_processor **out_xdp)
{
	(void)vk_cmd_pool;
	(void)window_handle;

	struct vk_bundle *vk = (struct vk_bundle *)vk_bundle_ptr;
	enum sim_display_output_mode mode = sim_display_get_output_mode();

	return sim_display_processor_create(mode, vk, target_format, out_xdp);
}
