// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Reference view synthesis (ADR-053 S5) on Vulkan — see the header.
 * @ingroup comp_vk_conv
 */

#include "vk_conv_viewsyn.h"

#include "vk/vk_helpers.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include "shaders/conv_viewsyn_prep.comp.h"
#include "shaders/conv_viewsyn_filter.comp.h"
#include "shaders/conv_viewsyn_guide.comp.h"
#include "shaders/conv_viewsyn_upsample.comp.h"
#include "shaders/conv_viewsyn_main.comp.h"

#include <math.h>
#include <string.h>

#define VS_LOCAL 8u
//! Descriptor sets one recording can use: prep 1, dilate 2, cores 2, guide 1,
//! upsample 1, dilate_hi 2, main 1 — with headroom.
#define VS_MAX_SETS 16u


/*
 *
 * Push constants (std430 offsets, byte-identical to the shaders' blocks).
 *
 */

struct pc_prep
{
	float value_scale, value_shift, near_lo, inv_span;
	uint32_t kind, has_invalid;
	float invalid_sample, pad0;
	int32_t out_size[2];
};

struct pc_filter
{
	int32_t dir[2];
	int32_t rad_r, rad_c;
	int32_t seed, pad0;
	int32_t out_size[2];
};

struct pc_guide
{
	float out_res[2];
};

struct pc_up
{
	float out_res[2];
	float sigma_s, sigma_r;
	int32_t snap, snap_r;
	float snap_tau, pad0;
};

struct pc_main
{
	float vp_size[2];
	float disp_res[2];
	float conv, qscale, taper, aspect, rear_k;
	int32_t steps, debug;
	float pad0;
	float eye[4];
	float cam[4];
};

_Static_assert(sizeof(struct pc_prep) == 40, "pc_prep");
_Static_assert(sizeof(struct pc_filter) == 32, "pc_filter");
_Static_assert(sizeof(struct pc_guide) == 8, "pc_guide");
_Static_assert(sizeof(struct pc_up) == 32, "pc_up");
_Static_assert(sizeof(struct pc_main) == 80, "pc_main");


/*
 *
 * Intermediates.
 *
 */

//! A storage + sampled image kept in VK_IMAGE_LAYOUT_GENERAL.
struct vs_tex
{
	VkImage image;
	VkDeviceMemory mem;
	VkImageView view;
	uint32_t w, h;
	VkFormat format;
	bool fresh; //!< created since the last recording: needs UNDEFINED -> GENERAL
};

//! Everything passes 1-6 read; a change re-runs them (the D3D11 stage's prepass_key).
struct prepass_key
{
	VkImageView color, depth;
	uint32_t color_w, color_h, depth_w, depth_h;
	uint32_t kind;
	float value_scale, value_shift, invalid_sample;
	double near_lo, near_hi;
	uint32_t dilate, dilate_hi, upsample_max, core, radius;
	float sigma_s, sigma_r, snap_tau;
	bool upsample, snap;
};

struct vk_conv_viewsyn
{
	struct vk_bundle *vk;

	VkSampler sampler; //!< linear, clamp to edge, no mips (live-dibr.js)

	VkDescriptorSetLayout dsl_a;    //!< 0 sampler, 1 storage (prep, filter, guide)
	VkDescriptorSetLayout dsl_up;   //!< 0..2 samplers, 3 storage
	VkDescriptorSetLayout dsl_main; //!< 0..1 samplers, 2 storage
	VkPipelineLayout pl_a, pl_up, pl_main;
	VkPipeline p_prep, p_filter, p_guide, p_up, p_main;
	VkDescriptorPool pool;

	struct vs_tex dlo_a, dlo_b; //!< depth res RGBA16F ping-pong: r = u, g = min core, b = max core
	struct vs_tex guide;        //!< depth res RGBA8
	struct vs_tex up_a, up_b;   //!< colour res (capped) RGBA16F ping-pong, `.r` only
	struct vs_tex view;         //!< one view, RGBA16F, blitted into the target
	struct vs_tex *disp;        //!< borrowed: what the main pass samples

	bool have_key;
	struct prepass_key key;

	uint64_t prepasses;
};


/*
 *
 * Helpers.
 *
 */

static bool
feq(float x, float y)
{
	return x == y || (isnan(x) && isnan(y));
}

static bool
key_equal(const struct prepass_key *a, const struct prepass_key *b)
{
	// Field-wise: a NaN invalid_sample must compare equal to itself.
	return a->color == b->color && a->depth == b->depth && a->color_w == b->color_w && a->color_h == b->color_h &&
	       a->depth_w == b->depth_w && a->depth_h == b->depth_h && a->kind == b->kind &&
	       feq(a->value_scale, b->value_scale) && feq(a->value_shift, b->value_shift) &&
	       feq(a->invalid_sample, b->invalid_sample) && a->near_lo == b->near_lo && a->near_hi == b->near_hi &&
	       a->dilate == b->dilate && a->dilate_hi == b->dilate_hi && a->upsample_max == b->upsample_max &&
	       a->core == b->core && a->radius == b->radius && a->sigma_s == b->sigma_s && a->sigma_r == b->sigma_r &&
	       a->snap_tau == b->snap_tau && a->upsample == b->upsample && a->snap == b->snap;
}

static void
tex_reset(struct vk_bundle *vk, struct vs_tex *t)
{
	if (t->view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, t->view, NULL);
	}
	if (t->image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, t->image, NULL);
	}
	if (t->mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, t->mem, NULL);
	}
	U_ZERO(t);
}

static bool
tex_ensure(struct vk_bundle *vk, struct vs_tex *t, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags extra)
{
	if (t->image != VK_NULL_HANDLE && t->w == w && t->h == h && t->format == format) {
		return true;
	}
	tex_reset(vk, t);
	VkImageCreateInfo ici = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = format,
	    .extent = {w, h, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extra,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vk->vkCreateImage(vk->device, &ici, NULL, &t->image) != VK_SUCCESS) {
		t->image = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryRequirements req;
	vk->vkGetImageMemoryRequirements(vk->device, t->image, &req);
	if (vk_alloc_and_bind_image_memory(vk, t->image, &req, NULL, "vk_conv_viewsyn", &t->mem) != VK_SUCCESS) {
		tex_reset(vk, t);
		return false;
	}
	VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (vk_create_view(vk, t->image, VK_IMAGE_VIEW_TYPE_2D, format, range, &t->view) != VK_SUCCESS) {
		tex_reset(vk, t);
		return false;
	}
	t->w = w;
	t->h = h;
	t->format = format;
	t->fresh = true;
	return true;
}

//! First use of a freshly created intermediate: UNDEFINED -> GENERAL.
static void
tex_prepare(struct vk_bundle *vk, VkCommandBuffer cmd, struct vs_tex *t)
{
	if (!t->fresh) {
		return;
	}
	VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vk_cmd_image_barrier_locked(vk, cmd, t->image, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
	                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
	                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, range);
	t->fresh = false;
}

//! Compute -> compute (RAW and WAR between passes).
static void
barrier_compute(struct vk_bundle *vk, VkCommandBuffer cmd)
{
	VkMemoryBarrier mb = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
	                         &mb, 0, NULL, 0, NULL);
}

static VkDescriptorSet
alloc_set(struct vk_conv_viewsyn *v, VkDescriptorSetLayout dsl)
{
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkDescriptorSetAllocateInfo ai = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	    .descriptorPool = v->pool,
	    .descriptorSetCount = 1,
	    .pSetLayouts = &dsl,
	};
	if (v->vk->vkAllocateDescriptorSets(v->vk->device, &ai, &set) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	return set;
}

/*!
 * Fill @p set: @p sampled[i] at bindings 0..n-1 (layout @p layouts[i]), the
 * storage image @p storage at binding n (GENERAL).
 */
static void
write_set(struct vk_conv_viewsyn *v,
          VkDescriptorSet set,
          const VkImageView *sampled,
          const VkImageLayout *layouts,
          uint32_t n,
          VkImageView storage)
{
	VkDescriptorImageInfo ii[4];
	VkWriteDescriptorSet w[4];
	memset(w, 0, sizeof(w));
	for (uint32_t i = 0; i < n; i++) {
		ii[i].sampler = v->sampler;
		ii[i].imageView = sampled[i];
		ii[i].imageLayout = layouts[i];
		w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[i].dstSet = set;
		w[i].dstBinding = i;
		w[i].descriptorCount = 1;
		w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w[i].pImageInfo = &ii[i];
	}
	ii[n].sampler = VK_NULL_HANDLE;
	ii[n].imageView = storage;
	ii[n].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	w[n].dstSet = set;
	w[n].dstBinding = n;
	w[n].descriptorCount = 1;
	w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	w[n].pImageInfo = &ii[n];
	v->vk->vkUpdateDescriptorSets(v->vk->device, n + 1, w, 0, NULL);
}

static void
dispatch(struct vk_conv_viewsyn *v,
         VkCommandBuffer cmd,
         VkPipeline p,
         VkPipelineLayout pl,
         VkDescriptorSet set,
         const void *pc,
         uint32_t pc_size,
         uint32_t w,
         uint32_t h)
{
	struct vk_bundle *vk = v->vk;
	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
	vk->vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, pc_size, pc);
	vk->vkCmdDispatch(cmd, (w + VS_LOCAL - 1) / VS_LOCAL, (h + VS_LOCAL - 1) / VS_LOCAL, 1);
}

//! One full-raster pass with ONE sampled source (GENERAL) into @p dst.
static bool
pass_1src(struct vk_conv_viewsyn *v,
          VkCommandBuffer cmd,
          VkPipeline p,
          VkImageView src,
          VkImageLayout src_layout,
          struct vs_tex *dst,
          const void *pc,
          uint32_t pc_size)
{
	VkDescriptorSet set = alloc_set(v, v->dsl_a);
	if (set == VK_NULL_HANDLE) {
		return false;
	}
	write_set(v, set, &src, &src_layout, 1, dst->view);
	dispatch(v, cmd, p, v->pl_a, set, pc, pc_size, dst->w, dst->h);
	barrier_compute(v->vk, cmd);
	return true;
}

//! Separable filter, both directions: src -> tmp (H) -> src (V). Result in @p src.
static bool
filter2(struct vk_conv_viewsyn *v,
        VkCommandBuffer cmd,
        struct vs_tex *src,
        struct vs_tex *tmp,
        int rad_r,
        int rad_c,
        bool seed)
{
	struct pc_filter c;
	memset(&c, 0, sizeof(c));
	c.rad_r = rad_r;
	c.rad_c = rad_c;
	c.dir[0] = 1;
	c.dir[1] = 0;
	c.seed = seed ? 1 : 0;
	c.out_size[0] = (int32_t)tmp->w;
	c.out_size[1] = (int32_t)tmp->h;
	if (!pass_1src(v, cmd, v->p_filter, src->view, VK_IMAGE_LAYOUT_GENERAL, tmp, &c, sizeof(c))) {
		return false;
	}
	c.dir[0] = 0;
	c.dir[1] = 1;
	c.seed = 0; // the H pass already seeded g / b
	return pass_1src(v, cmd, v->p_filter, tmp->view, VK_IMAGE_LAYOUT_GENERAL, src, &c, sizeof(c));
}

static bool
make_pipeline(struct vk_bundle *vk, const uint32_t *code, size_t size, VkPipelineLayout pl, VkPipeline *out)
{
	VkShaderModuleCreateInfo smci = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = size,
	    .pCode = code,
	};
	VkShaderModule sm = VK_NULL_HANDLE;
	if (vk->vkCreateShaderModule(vk->device, &smci, NULL, &sm) != VK_SUCCESS) {
		return false;
	}
	VkResult r = vk_create_compute_pipeline(vk, VK_NULL_HANDLE, sm, pl, NULL, out);
	vk->vkDestroyShaderModule(vk->device, sm, NULL);
	return r == VK_SUCCESS;
}

static bool
make_layout(struct vk_bundle *vk, uint32_t samplers, VkDescriptorSetLayout *dsl, VkPipelineLayout *pl)
{
	VkDescriptorSetLayoutBinding b[4];
	memset(b, 0, sizeof(b));
	for (uint32_t i = 0; i <= samplers; i++) {
		b[i].binding = i;
		b[i].descriptorType =
		    i < samplers ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo dci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = samplers + 1,
	    .pBindings = b,
	};
	if (vk->vkCreateDescriptorSetLayout(vk->device, &dci, NULL, dsl) != VK_SUCCESS) {
		return false;
	}
	VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
	VkPipelineLayoutCreateInfo pci = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount = 1,
	    .pSetLayouts = dsl,
	    .pushConstantRangeCount = 1,
	    .pPushConstantRanges = &range,
	};
	return vk->vkCreatePipelineLayout(vk->device, &pci, NULL, pl) == VK_SUCCESS;
}

static bool
format_blit_dst(struct vk_bundle *vk, VkFormat f)
{
	VkFormatProperties fp;
	vk->vkGetPhysicalDeviceFormatProperties(vk->physical_device, f, &fp);
	return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
}


/*
 *
 * Exported functions.
 *
 */

struct vk_conv_viewsyn *
vk_conv_viewsyn_create(struct vk_bundle *vk)
{
	if (vk == NULL || vk->device == VK_NULL_HANDLE) {
		return NULL;
	}
	struct vk_conv_viewsyn *v = U_TYPED_CALLOC(struct vk_conv_viewsyn);
	v->vk = vk;

	// live-dibr.js: LINEAR min/mag, CLAMP_TO_EDGE, no mips.
	VkSamplerCreateInfo sci = {
	    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	    .magFilter = VK_FILTER_LINEAR,
	    .minFilter = VK_FILTER_LINEAR,
	    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
	    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .maxLod = 0.0f,
	    .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
	};
	bool ok = vk->vkCreateSampler(vk->device, &sci, NULL, &v->sampler) == VK_SUCCESS;
	ok = ok && make_layout(vk, 1, &v->dsl_a, &v->pl_a);
	ok = ok && make_layout(vk, 3, &v->dsl_up, &v->pl_up);
	ok = ok && make_layout(vk, 2, &v->dsl_main, &v->pl_main);
	ok = ok && make_pipeline(vk, shaders_conv_viewsyn_prep_comp, sizeof(shaders_conv_viewsyn_prep_comp), v->pl_a,
	                         &v->p_prep);
	ok = ok && make_pipeline(vk, shaders_conv_viewsyn_filter_comp, sizeof(shaders_conv_viewsyn_filter_comp),
	                         v->pl_a, &v->p_filter);
	ok = ok && make_pipeline(vk, shaders_conv_viewsyn_guide_comp, sizeof(shaders_conv_viewsyn_guide_comp), v->pl_a,
	                         &v->p_guide);
	ok = ok && make_pipeline(vk, shaders_conv_viewsyn_upsample_comp, sizeof(shaders_conv_viewsyn_upsample_comp),
	                         v->pl_up, &v->p_up);
	ok = ok && make_pipeline(vk, shaders_conv_viewsyn_main_comp, sizeof(shaders_conv_viewsyn_main_comp), v->pl_main,
	                         &v->p_main);

	VkDescriptorPoolSize sizes[2] = {
	    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VS_MAX_SETS * 3},
	    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VS_MAX_SETS},
	};
	VkDescriptorPoolCreateInfo dpci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	    .maxSets = VS_MAX_SETS,
	    .poolSizeCount = 2,
	    .pPoolSizes = sizes,
	};
	ok = ok && vk->vkCreateDescriptorPool(vk->device, &dpci, NULL, &v->pool) == VK_SUCCESS;

	if (!ok) {
		U_LOG_E("[viewsyn] Vulkan view-synthesis stage init failed");
		vk_conv_viewsyn_destroy(&v);
		return NULL;
	}
	return v;
}

void
vk_conv_viewsyn_destroy(struct vk_conv_viewsyn **vs_ptr)
{
	if (vs_ptr == NULL || *vs_ptr == NULL) {
		return;
	}
	struct vk_conv_viewsyn *v = *vs_ptr;
	struct vk_bundle *vk = v->vk;
	tex_reset(vk, &v->dlo_a);
	tex_reset(vk, &v->dlo_b);
	tex_reset(vk, &v->guide);
	tex_reset(vk, &v->up_a);
	tex_reset(vk, &v->up_b);
	tex_reset(vk, &v->view);
	VkPipeline ps[5] = {v->p_prep, v->p_filter, v->p_guide, v->p_up, v->p_main};
	for (int i = 0; i < 5; i++) {
		if (ps[i] != VK_NULL_HANDLE) {
			vk->vkDestroyPipeline(vk->device, ps[i], NULL);
		}
	}
	VkPipelineLayout pls[3] = {v->pl_a, v->pl_up, v->pl_main};
	VkDescriptorSetLayout dsls[3] = {v->dsl_a, v->dsl_up, v->dsl_main};
	for (int i = 0; i < 3; i++) {
		if (pls[i] != VK_NULL_HANDLE) {
			vk->vkDestroyPipelineLayout(vk->device, pls[i], NULL);
		}
		if (dsls[i] != VK_NULL_HANDLE) {
			vk->vkDestroyDescriptorSetLayout(vk->device, dsls[i], NULL);
		}
	}
	if (v->pool != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorPool(vk->device, v->pool, NULL);
	}
	if (v->sampler != VK_NULL_HANDLE) {
		vk->vkDestroySampler(vk->device, v->sampler, NULL);
	}
	free(v);
	*vs_ptr = NULL;
}

void
vk_conv_viewsyn_invalidate(struct vk_conv_viewsyn *vs)
{
	if (vs != NULL) {
		vs->have_key = false;
	}
}

uint64_t
vk_conv_viewsyn_prepass_count(const struct vk_conv_viewsyn *vs)
{
	return vs != NULL ? vs->prepasses : 0;
}

static bool
run_prepasses(struct vk_conv_viewsyn *v,
              VkCommandBuffer cmd,
              const struct vk_conv_viewsyn_inputs *in,
              const struct u_conv_viewsyn_params *p,
              const struct u_conv_viewsyn_frame *f,
              const struct prepass_key *k)
{
	struct vk_bundle *vk = v->vk;
	const uint32_t dw = in->depth_width, dh = in->depth_height;
	if (!tex_ensure(vk, &v->dlo_a, dw, dh, VK_FORMAT_R16G16B16A16_SFLOAT, 0) ||
	    !tex_ensure(vk, &v->dlo_b, dw, dh, VK_FORMAT_R16G16B16A16_SFLOAT, 0)) {
		return false;
	}
	tex_prepare(vk, cmd, &v->dlo_a);
	tex_prepare(vk, cmd, &v->dlo_b);

	// 1. prep: raw -> normalised nearness (r = g = b = u).
	struct pc_prep cp;
	memset(&cp, 0, sizeof(cp));
	cp.value_scale = p->depth.value_scale == 0.0f ? 1.0f : p->depth.value_scale;
	cp.value_shift = p->depth.value_shift;
	cp.near_lo = (float)f->near_lo;
	cp.inv_span = (float)(1.0 / (f->near_hi - f->near_lo));
	cp.kind = p->depth.kind;
	cp.has_invalid = isnan(p->depth.invalid_sample) ? 0u : 1u;
	cp.invalid_sample = cp.has_invalid ? p->depth.invalid_sample : 0.0f;
	cp.out_size[0] = (int32_t)dw;
	cp.out_size[1] = (int32_t)dh;
	if (!pass_1src(v, cmd, v->p_prep, in->depth, in->input_layout, &v->dlo_a, &cp, sizeof(cp))) {
		return false;
	}

	// 2. foreground dilation at depth res (lift() ships 0).
	if (k->dilate > 0 && !filter2(v, cmd, &v->dlo_a, &v->dlo_b, (int)k->dilate, 0, false)) {
		return false;
	}
	// 3. the snap's cores (live-dibr.js snapPack: core 0 when the snap is off).
	if (k->core > 0) {
		if (!filter2(v, cmd, &v->dlo_a, &v->dlo_b, 0, (int)k->core, true)) {
			return false;
		}
	} else if (k->dilate > 0) {
		// g / b must follow the dilated r: re-seed with a zero-radius pass.
		if (!filter2(v, cmd, &v->dlo_a, &v->dlo_b, 0, 0, true)) {
			return false;
		}
	}
	v->disp = &v->dlo_a;

	if (k->upsample) {
		// 4. guide: the colour at depth res.
		if (!tex_ensure(vk, &v->guide, dw, dh, VK_FORMAT_R8G8B8A8_UNORM, 0)) {
			return false;
		}
		tex_prepare(vk, cmd, &v->guide);
		struct pc_guide cg = {{(float)dw, (float)dh}};
		if (!pass_1src(v, cmd, v->p_guide, in->color, in->input_layout, &v->guide, &cg, sizeof(cg))) {
			return false;
		}

		// 5. JBU + edge snap to (capped) colour res.
		uint32_t uw, uh;
		u_conv_viewsyn_upsample_size(in->color_width, in->color_height, k->upsample_max, &uw, &uh);
		if (!tex_ensure(vk, &v->up_a, uw, uh, VK_FORMAT_R16G16B16A16_SFLOAT, 0) ||
		    !tex_ensure(vk, &v->up_b, uw, uh, VK_FORMAT_R16G16B16A16_SFLOAT, 0)) {
			return false;
		}
		tex_prepare(vk, cmd, &v->up_a);
		tex_prepare(vk, cmd, &v->up_b);
		struct pc_up cu;
		memset(&cu, 0, sizeof(cu));
		cu.out_res[0] = (float)uw;
		cu.out_res[1] = (float)uh;
		cu.sigma_s = fmaxf(0.1f, k->sigma_s > 0.0f ? k->sigma_s : 1.0f);
		cu.sigma_r = fmaxf(0.005f, k->sigma_r > 0.0f ? k->sigma_r : 0.1f);
		cu.snap = k->snap ? 1 : 0;
		cu.snap_r = (int32_t)k->radius;
		cu.snap_tau = fmaxf(1e-6f, k->snap_tau); // normalised: (hi - lo) == 1
		VkDescriptorSet set = alloc_set(v, v->dsl_up);
		if (set == VK_NULL_HANDLE) {
			return false;
		}
		VkImageView srcs[3] = {v->dlo_a.view, v->guide.view, in->color};
		VkImageLayout lays[3] = {VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, in->input_layout};
		write_set(v, set, srcs, lays, 3, v->up_a.view);
		dispatch(v, cmd, v->p_up, v->pl_up, set, &cu, sizeof(cu), uw, uh);
		barrier_compute(vk, cmd);

		// 6. optional max filter after the upsample.
		if (k->dilate_hi > 0 && !filter2(v, cmd, &v->up_a, &v->up_b, (int)k->dilate_hi, 0, false)) {
			return false;
		}
		v->disp = &v->up_a;
	}
	v->prepasses++;
	return true;
}

bool
vk_conv_viewsyn_record(struct vk_conv_viewsyn *v,
                       VkCommandBuffer cmd,
                       const struct vk_conv_viewsyn_inputs *in,
                       const struct u_conv_viewsyn_params *p,
                       const struct vk_conv_viewsyn_target *t)
{
	if (v == NULL || cmd == VK_NULL_HANDLE || in == NULL || p == NULL || t == NULL || in->color == VK_NULL_HANDLE ||
	    in->depth == VK_NULL_HANDLE || t->image == VK_NULL_HANDLE || in->depth_width == 0 ||
	    in->depth_height == 0 || in->color_width == 0 || in->color_height == 0) {
		return false;
	}
	struct vk_bundle *vk = v->vk;
	struct u_conv_viewsyn_frame f;
	if (!u_conv_viewsyn_resolve(p, &f)) {
		return false;
	}
	uint32_t vw = 0, vh = 0;
	if (t->layout_kind == XRT_CONV_VIEWS_ROW) {
		if (t->width < f.view_count) {
			return false;
		}
		vw = t->width / f.view_count;
		vh = t->height;
	} else if (t->layout_kind == XRT_CONV_VIEWS_ARRAY) {
		if (t->array_layers < f.view_count) {
			return false;
		}
		vw = t->width;
		vh = t->height;
	} else {
		return false;
	}
	if (vw == 0 || vh == 0 || !format_blit_dst(vk, t->format)) {
		return false;
	}
	if (!tex_ensure(vk, &v->view, vw, vh, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
		return false;
	}

	struct prepass_key k;
	memset(&k, 0, sizeof(k));
	k.color = in->color;
	k.depth = in->depth;
	k.color_w = in->color_width;
	k.color_h = in->color_height;
	k.depth_w = in->depth_width;
	k.depth_h = in->depth_height;
	k.kind = p->depth.kind;
	k.value_scale = p->depth.value_scale;
	k.value_shift = p->depth.value_shift;
	k.invalid_sample = p->depth.invalid_sample;
	k.near_lo = f.near_lo;
	k.near_hi = f.near_hi;
	k.dilate = p->dilate > 16 ? 16 : p->dilate;
	k.upsample = p->upsample;
	k.snap = p->upsample && p->snap;
	k.dilate_hi = p->upsample ? (p->dilate_hi > 8 ? 8 : p->dilate_hi) : 0;
	k.upsample_max = p->upsample_max > 0 ? p->upsample_max : 1;
	k.sigma_s = p->upsample_sigma_s;
	k.sigma_r = p->upsample_sigma_r;
	k.snap_tau = p->snap_tau;
	if (k.snap) {
		u_conv_viewsyn_snap_radii(p->snap_core, p->snap_radius, in->depth_width, &k.core, &k.radius);
		if (k.core > 16) {
			k.core = 16;
		}
	}

	// Everything earlier on the queue (other submissions included) happens
	// before any pass here reads or overwrites: the inputs, the cached
	// intermediates, the target.
	VkMemoryBarrier mb = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
	                     VK_ACCESS_TRANSFER_WRITE_BIT,
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
	                         NULL, 0, NULL);
	vk->vkResetDescriptorPool(vk->device, v->pool, 0);

	if (in->new_content || !v->have_key || !key_equal(&k, &v->key)) {
		if (!run_prepasses(v, cmd, in, p, &f, &k)) {
			v->have_key = false;
			return false;
		}
		v->key = k;
		v->have_key = true;
	}
	tex_prepare(vk, cmd, &v->view);

	// The target: whatever it held -> GENERAL, as a transfer destination.
	const uint32_t layers = t->layout_kind == XRT_CONV_VIEWS_ARRAY ? t->array_layers : 1;
	VkImageSubresourceRange trange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
	vk_cmd_image_barrier_locked(vk, cmd, t->image, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
	                            VK_ACCESS_TRANSFER_WRITE_BIT, t->layout, VK_IMAGE_LAYOUT_GENERAL,
	                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, trange);

	// 7. one dispatch per view, then a blit into its place.
	VkDescriptorSet set = alloc_set(v, v->dsl_main);
	if (set == VK_NULL_HANDLE) {
		v->have_key = false;
		return false;
	}
	VkImageView srcs[2] = {in->color, v->disp->view};
	VkImageLayout lays[2] = {in->input_layout, VK_IMAGE_LAYOUT_GENERAL};
	write_set(v, set, srcs, lays, 2, v->view.view);

	struct pc_main c;
	memset(&c, 0, sizeof(c));
	c.vp_size[0] = (float)vw;
	c.vp_size[1] = (float)vh;
	c.disp_res[0] = (float)in->depth_width;
	c.disp_res[1] = (float)in->depth_height;
	c.conv = (float)f.conv_n;
	c.taper = (float)f.taper;
	c.aspect = (float)f.aspect;
	c.rear_k = (float)f.rear_k;
	c.steps = (int32_t)f.steps;
	c.debug = p->debug == 1 ? 1 : 0;
	c.cam[0] = (float)f.cam.x;
	c.cam[1] = (float)f.cam.y;
	c.cam[2] = (float)f.cam.z;
	for (uint32_t i = 0; i < f.view_count; i++) {
		const bool row = t->layout_kind == XRT_CONV_VIEWS_ROW;
		c.qscale = (float)f.views[i].q_scale;
		c.eye[0] = (float)f.views[i].eye.x;
		c.eye[1] = (float)f.views[i].eye.y;
		c.eye[2] = (float)f.views[i].eye.z;
		dispatch(v, cmd, v->p_main, v->pl_main, set, &c, sizeof(c), vw, vh);

		VkMemoryBarrier to_xfer = {
		    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		                         1, &to_xfer, 0, NULL, 0, NULL);
		VkImageBlit blit = {
		    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		    .srcOffsets = {{0, 0, 0}, {(int32_t)vw, (int32_t)vh, 1}},
		    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, row ? 0 : i, 1},
		    .dstOffsets = {{row ? (int32_t)(i * vw) : 0, 0, 0},
		                   {row ? (int32_t)((i + 1) * vw) : (int32_t)vw, (int32_t)vh, 1}},
		};
		vk->vkCmdBlitImage(cmd, v->view.image, VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL, 1,
		                   &blit, VK_FILTER_NEAREST);
		// The next view's dispatch overwrites the scratch the blit read.
		VkMemoryBarrier to_comp = {
		    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		    .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
		                         1, &to_comp, 0, NULL, 0, NULL);
	}

	// The views are visible to whatever runs next on the queue.
	VkMemoryBarrier done = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &done,
	                         0, NULL, 0, NULL);
	return true;
}
