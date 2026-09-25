// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1602 — the vk_native equirect2 shaders map rays onto the sphere and
 *         cover only the layer's angular extent. The Vulkan twin of
 *         tests_comp_layer_equirect2_warp.cpp.
 * @ingroup tests
 *
 * Same three discriminators as the D3D11 WARP test, asked of the SPIR-V the
 * vk_native compose pass actually binds (the build's generated
 * shaders/equirect2.{vert,frag}.h) and of the CPU packing it actually uses
 * (comp_vk_native_equirect2.h):
 *
 *  0. DID ANYTHING DRAW — checked first and on its own, against a magenta clear
 *     nothing here can produce, so "never covered the pixel" (pipeline state)
 *     is not confused with "sampled the wrong texel" (shader math).
 *  1. THE HUE PAIR, BOTH DIRECTIONS — the source is split by latitude, top red
 *     and bottom green; a pixel above the tile centre must read red and one
 *     below green. This pins the screen-to-ray Y orientation, which is exactly
 *     where Vulkan differs from D3D/GL (NDC +y is the BOTTOM), and which a
 *     packing error in the ray basis would swap.
 *  2. THE ANGULAR EXTENT — with a 45° centralHorizontalAngle the edge columns
 *     are outside the layer and must keep the clear under the blending-OFF
 *     state an unflagged layer gets. Only `discard` does that; returning
 *     transparent black overwrites the tile.
 *
 * Everything the renderer binds is reproduced — CULL_NONE, triangle list of 3,
 * blending off, one 128-byte push-constant range visible to both stages, the
 * dynamic viewport/scissor — because anything left at a default is a latent
 * "the oracle read its own clear" failure.
 *
 * Prefers a CPU (lavapipe) device, so the answer does not depend on the box's
 * GPU; takes any device otherwise, and SKIPs when there is none at all.
 */

#include "vk_native/comp_vk_native_equirect2.h"

#include "shaders/equirect2.vert.h"
#include "shaders/equirect2.frag.h"

#include "catch_amalgamated.hpp"

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kDim = 16;
constexpr float kPi = 3.14159265358979323846f;

struct Rgba
{
	uint8_t r, g, b, a;

	bool
	operator==(const Rgba &o) const
	{
		return r == o.r && g == o.g && b == o.b && a == o.a;
	}
};

constexpr Rgba kTop = {255, 32, 0, 255};    // red — the texture's TOP half
constexpr Rgba kBottom = {0, 208, 64, 255}; // green — its BOTTOM half
constexpr Rgba kClear = {255, 0, 255, 255}; // magenta — nothing else makes it

std::ostream &
operator<<(std::ostream &os, const Rgba &c)
{
	return os << "(" << int(c.r) << "," << int(c.g) << "," << int(c.b) << "," << int(c.a) << ")";
}

//! Byte-identical to vk_compose_push in comp_vk_native_renderer.c.
struct Push
{
	float eq[16];
	float src_rect[4];
	float params[4];
	float color_scale[4];
	float color_bias[4];
};
static_assert(sizeof(Push) == 128, "the compose pass's push block is exactly 128 bytes");

struct Fixture
{
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice dev = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t qfi = 0;
	VkCommandPool pool = VK_NULL_HANDLE;

	VkImage src = VK_NULL_HANDLE;
	VkDeviceMemory src_mem = VK_NULL_HANDLE;
	VkImageView src_view = VK_NULL_HANDLE;
	VkImage rt = VK_NULL_HANDLE;
	VkDeviceMemory rt_mem = VK_NULL_HANDLE;
	VkImageView rt_view = VK_NULL_HANDLE;
	VkBuffer buf = VK_NULL_HANDLE;
	VkDeviceMemory buf_mem = VK_NULL_HANDLE;

	VkSampler sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkDescriptorPool dpool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkRenderPass rp = VK_NULL_HANDLE;
	VkFramebuffer fb = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;

	~Fixture()
	{
		if (dev != VK_NULL_HANDLE) {
			vkDeviceWaitIdle(dev);
			vkDestroyPipeline(dev, pipeline, nullptr);
			vkDestroyFramebuffer(dev, fb, nullptr);
			vkDestroyRenderPass(dev, rp, nullptr);
			vkDestroyDescriptorPool(dev, dpool, nullptr);
			vkDestroyPipelineLayout(dev, layout, nullptr);
			vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
			vkDestroySampler(dev, sampler, nullptr);
			vkDestroyBuffer(dev, buf, nullptr);
			vkFreeMemory(dev, buf_mem, nullptr);
			vkDestroyImageView(dev, rt_view, nullptr);
			vkDestroyImage(dev, rt, nullptr);
			vkFreeMemory(dev, rt_mem, nullptr);
			vkDestroyImageView(dev, src_view, nullptr);
			vkDestroyImage(dev, src, nullptr);
			vkFreeMemory(dev, src_mem, nullptr);
			vkDestroyCommandPool(dev, pool, nullptr);
			vkDestroyDevice(dev, nullptr);
		}
		if (instance != VK_NULL_HANDLE) {
			vkDestroyInstance(instance, nullptr);
		}
	}
};

uint32_t
memory_type(Fixture &f, uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(f.phys, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		if ((bits & (1u << i)) != 0 && (mp.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	FAIL("no memory type for bits " << bits);
	return 0;
}

void
make_image(Fixture &f, VkImageUsageFlags usage, VkImage *img, VkDeviceMemory *mem, VkImageView *view)
{
	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent = {kDim, kDim, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = usage;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	REQUIRE(vkCreateImage(f.dev, &ici, nullptr, img) == VK_SUCCESS);

	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(f.dev, *img, &mr);
	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = memory_type(f, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	REQUIRE(vkAllocateMemory(f.dev, &mai, nullptr, mem) == VK_SUCCESS);
	REQUIRE(vkBindImageMemory(f.dev, *img, *mem, 0) == VK_SUCCESS);

	VkImageViewCreateInfo vci = {};
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = *img;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = VK_FORMAT_R8G8B8A8_UNORM;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	REQUIRE(vkCreateImageView(f.dev, &vci, nullptr, view) == VK_SUCCESS);
}

void
barrier(VkCommandBuffer cmd,
        VkImage img,
        VkImageLayout from,
        VkImageLayout to,
        VkAccessFlags src_access,
        VkAccessFlags dst_access,
        VkPipelineStageFlags src_stage,
        VkPipelineStageFlags dst_stage)
{
	VkImageMemoryBarrier b = {};
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.srcAccessMask = src_access;
	b.dstAccessMask = dst_access;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkCommandBuffer
begin(Fixture &f)
{
	VkCommandBufferAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	ai.commandPool = f.pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	REQUIRE(vkAllocateCommandBuffers(f.dev, &ai, &cmd) == VK_SUCCESS);
	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);
	return cmd;
}

void
submit(Fixture &f, VkCommandBuffer cmd)
{
	vkEndCommandBuffer(cmd);
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	REQUIRE(vkQueueSubmit(f.queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS);
	REQUIRE(vkQueueWaitIdle(f.queue) == VK_SUCCESS);
	vkFreeCommandBuffers(f.dev, f.pool, 1, &cmd);
}

//! @return false only when no Vulkan device exists at all → the caller SKIPs.
bool
setup(Fixture &f)
{
	VkApplicationInfo app = {};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "tests_comp_vk_equirect2_draw";
	app.apiVersion = VK_API_VERSION_1_0;
	VkInstanceCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.pApplicationInfo = &app;
	if (vkCreateInstance(&ici, nullptr, &f.instance) != VK_SUCCESS) {
		return false;
	}

	uint32_t n = 0;
	vkEnumeratePhysicalDevices(f.instance, &n, nullptr);
	if (n == 0) {
		return false;
	}
	std::vector<VkPhysicalDevice> devs(n);
	vkEnumeratePhysicalDevices(f.instance, &n, devs.data());

	// A CPU (lavapipe) device first: deterministic and GPU-independent, the
	// Vulkan counterpart of the D3D test's WARP-first rule.
	f.phys = devs[0];
	for (VkPhysicalDevice d : devs) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(d, &p);
		if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
			f.phys = d;
			break;
		}
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(f.phys, &props);
	UNSCOPED_INFO("Vulkan device: " << props.deviceName);
	REQUIRE(props.limits.maxPushConstantsSize >= sizeof(Push));

	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(f.phys, &qn, nullptr);
	std::vector<VkQueueFamilyProperties> qf(qn);
	vkGetPhysicalDeviceQueueFamilyProperties(f.phys, &qn, qf.data());
	bool found = false;
	for (uint32_t i = 0; i < qn; i++) {
		if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
			f.qfi = i;
			found = true;
			break;
		}
	}
	if (!found) {
		return false;
	}

	const float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {};
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = f.qfi;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	REQUIRE(vkCreateDevice(f.phys, &dci, nullptr, &f.dev) == VK_SUCCESS);
	vkGetDeviceQueue(f.dev, f.qfi, 0, &f.queue);

	VkCommandPoolCreateInfo pci = {};
	pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pci.queueFamilyIndex = f.qfi;
	REQUIRE(vkCreateCommandPool(f.dev, &pci, nullptr, &f.pool) == VK_SUCCESS);

	make_image(f, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &f.src, &f.src_mem, &f.src_view);
	make_image(f, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &f.rt, &f.rt_mem,
	           &f.rt_view);

	// One host-visible buffer serves as the upload source and the readback
	// target: both are kDim*kDim RGBA8.
	VkBufferCreateInfo bci = {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = kDim * kDim * sizeof(Rgba);
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	REQUIRE(vkCreateBuffer(f.dev, &bci, nullptr, &f.buf) == VK_SUCCESS);
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(f.dev, f.buf, &mr);
	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = memory_type(f, mr.memoryTypeBits,
	                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	REQUIRE(vkAllocateMemory(f.dev, &mai, nullptr, &f.buf_mem) == VK_SUCCESS);
	REQUIRE(vkBindBufferMemory(f.dev, f.buf, f.buf_mem, 0) == VK_SUCCESS);

	// The equirect source: top half red, bottom half green. Row 0 is the TOP
	// row of a Vulkan image, and the shader's `lat` is 0 at the zenith, so this
	// is a latitude split and nothing else.
	void *p = nullptr;
	REQUIRE(vkMapMemory(f.dev, f.buf_mem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS);
	Rgba *texels = static_cast<Rgba *>(p);
	for (uint32_t y = 0; y < kDim; y++) {
		for (uint32_t x = 0; x < kDim; x++) {
			texels[y * kDim + x] = (y < kDim / 2) ? kTop : kBottom;
		}
	}
	vkUnmapMemory(f.dev, f.buf_mem);

	VkCommandBuffer cmd = begin(f);
	barrier(cmd, f.src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkBufferImageCopy region = {};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {kDim, kDim, 1};
	vkCmdCopyBufferToImage(cmd, f.buf, f.src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	barrier(cmd, f.src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	submit(f, cmd);

	// POINT filtering: every sample sits well away from the boundary row, so
	// filtering cannot change the answer, but point sampling removes the doubt.
	VkSamplerCreateInfo sci = {};
	sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sci.magFilter = VK_FILTER_NEAREST;
	sci.minFilter = VK_FILTER_NEAREST;
	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	REQUIRE(vkCreateSampler(f.dev, &sci, nullptr, &f.sampler) == VK_SUCCESS);

	// The renderer's descriptor layout and push range, verbatim.
	VkDescriptorSetLayoutBinding binding = {};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo dslci = {};
	dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dslci.bindingCount = 1;
	dslci.pBindings = &binding;
	REQUIRE(vkCreateDescriptorSetLayout(f.dev, &dslci, nullptr, &f.dsl) == VK_SUCCESS);

	VkPushConstantRange range = {};
	range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	range.size = sizeof(Push);
	VkPipelineLayoutCreateInfo plci = {};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &f.dsl;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &range;
	REQUIRE(vkCreatePipelineLayout(f.dev, &plci, nullptr, &f.layout) == VK_SUCCESS);

	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
	VkDescriptorPoolCreateInfo dpci = {};
	dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpci.maxSets = 1;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &ps;
	REQUIRE(vkCreateDescriptorPool(f.dev, &dpci, nullptr, &f.dpool) == VK_SUCCESS);
	VkDescriptorSetAllocateInfo dsai = {};
	dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool = f.dpool;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &f.dsl;
	REQUIRE(vkAllocateDescriptorSets(f.dev, &dsai, &f.set) == VK_SUCCESS);
	VkDescriptorImageInfo dii = {f.sampler, f.src_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet w = {};
	w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	w.dstSet = f.set;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = &dii;
	vkUpdateDescriptorSets(f.dev, 1, &w, 0, nullptr);

	// loadOp CLEAR to magenta; ends TRANSFER_SRC for the readback.
	VkAttachmentDescription att = {};
	att.format = VK_FORMAT_R8G8B8A8_UNORM;
	att.samples = VK_SAMPLE_COUNT_1_BIT;
	att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub = {};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &ref;
	VkSubpassDependency dep = {};
	dep.srcSubpass = 0;
	dep.dstSubpass = VK_SUBPASS_EXTERNAL;
	dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
	dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	VkRenderPassCreateInfo rpci = {};
	rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpci.attachmentCount = 1;
	rpci.pAttachments = &att;
	rpci.subpassCount = 1;
	rpci.pSubpasses = &sub;
	rpci.dependencyCount = 1;
	rpci.pDependencies = &dep;
	REQUIRE(vkCreateRenderPass(f.dev, &rpci, nullptr, &f.rp) == VK_SUCCESS);

	VkFramebufferCreateInfo fbci = {};
	fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fbci.renderPass = f.rp;
	fbci.attachmentCount = 1;
	fbci.pAttachments = &f.rt_view;
	fbci.width = kDim;
	fbci.height = kDim;
	fbci.layers = 1;
	REQUIRE(vkCreateFramebuffer(f.dev, &fbci, nullptr, &f.fb) == VK_SUCCESS);

	// The SPIR-V the renderer binds, from the same generated headers.
	VkShaderModule vert = VK_NULL_HANDLE;
	VkShaderModule frag = VK_NULL_HANDLE;
	VkShaderModuleCreateInfo smci = {};
	smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	smci.codeSize = sizeof(shaders_equirect2_vert);
	smci.pCode = shaders_equirect2_vert;
	REQUIRE(vkCreateShaderModule(f.dev, &smci, nullptr, &vert) == VK_SUCCESS);
	smci.codeSize = sizeof(shaders_equirect2_frag);
	smci.pCode = shaders_equirect2_frag;
	REQUIRE(vkCreateShaderModule(f.dev, &smci, nullptr, &frag) == VK_SUCCESS);

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
	VkPipelineViewportStateCreateInfo vps = {};
	vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	vps.viewportCount = 1;
	vps.scissorCount = 1;
	// CULL_NONE, as zone_create_pipeline() creates every compose pipeline.
	VkPipelineRasterizationStateCreateInfo rs = {};
	rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms = {};
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	// Blending OFF — the state REPLACE and OPAQUE_COVER share, and the one
	// under which a returned (0,0,0,0) would overwrite instead of composite.
	VkPipelineColorBlendAttachmentState cba = {};
	cba.blendEnable = VK_FALSE;
	cba.colorWriteMask =
	    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo cb = {};
	cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;
	VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo ds = {};
	ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dyn;
	VkGraphicsPipelineCreateInfo gpci = {};
	gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	gpci.stageCount = 2;
	gpci.pStages = stages;
	gpci.pVertexInputState = &vi;
	gpci.pInputAssemblyState = &ia;
	gpci.pViewportState = &vps;
	gpci.pRasterizationState = &rs;
	gpci.pMultisampleState = &ms;
	gpci.pColorBlendState = &cb;
	gpci.pDynamicState = &ds;
	gpci.layout = f.layout;
	gpci.renderPass = f.rp;
	const VkResult pres = vkCreateGraphicsPipelines(f.dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &f.pipeline);
	vkDestroyShaderModule(f.dev, vert, nullptr);
	vkDestroyShaderModule(f.dev, frag, nullptr);
	REQUIRE(pres == VK_SUCCESS);
	return true;
}

//! One equirect2 draw over the whole target, identity poses, a symmetric 90°
//! frustum. @p central_h is the layer's centralHorizontalAngle.
void
draw(Fixture &f, float radius, float central_h)
{
	// Identity inverse model-view: layer and view poses both identity, so the
	// camera sits at the sphere's centre looking down -Z. Built directly
	// rather than through the math helpers, so what is asserted is the PACKER
	// and the SHADER, not the draw site's matrix chain.
	struct xrt_matrix_4x4 mv_inv = {};
	mv_inv.v[0] = mv_inv.v[5] = mv_inv.v[10] = mv_inv.v[15] = 1.0f;
	const struct xrt_fov fov = {-kPi / 4.0f, kPi / 4.0f, kPi / 4.0f, -kPi / 4.0f};

	Push push = {};
	comp_vk_native_equirect2_pack(&mv_inv, &fov, radius, central_h, kPi / 2.0f, -kPi / 2.0f, push.eq);
	push.src_rect[2] = 1.0f; // identity sub-image
	push.src_rect[3] = 1.0f;
	push.color_scale[0] = push.color_scale[1] = push.color_scale[2] = 1.0f;
	// OPAQUE_COVER's alpha-of-one, folded as comp_layer_blend_fold_opaque_cover()
	// folds it for an unflagged layer.
	push.color_scale[3] = 0.0f;
	push.color_bias[3] = 1.0f;

	VkCommandBuffer cmd = begin(f);
	VkClearValue clear = {};
	clear.color.float32[0] = 1.0f;
	clear.color.float32[1] = 0.0f;
	clear.color.float32[2] = 1.0f;
	clear.color.float32[3] = 1.0f;
	VkRenderPassBeginInfo rpbi = {};
	rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rpbi.renderPass = f.rp;
	rpbi.framebuffer = f.fb;
	rpbi.renderArea = {{0, 0}, {kDim, kDim}};
	rpbi.clearValueCount = 1;
	rpbi.pClearValues = &clear;
	vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
	VkViewport vp = {0.0f, 0.0f, float(kDim), float(kDim), 0.0f, 1.0f};
	VkRect2D sc = {{0, 0}, {kDim, kDim}};
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, f.pipeline);
	vkCmdSetViewport(cmd, 0, 1, &vp);
	vkCmdSetScissor(cmd, 0, 1, &sc);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, f.layout, 0, 1, &f.set, 0, nullptr);
	vkCmdPushConstants(cmd, f.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
	                   &push);
	vkCmdDraw(cmd, 3, 1, 0, 0); // the renderer's equirect2 draw: one fullscreen triangle
	vkCmdEndRenderPass(cmd);

	VkBufferImageCopy region = {};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {kDim, kDim, 1};
	vkCmdCopyImageToBuffer(cmd, f.rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f.buf, 1, &region);
	submit(f, cmd);
}

Rgba
pixel(Fixture &f, uint32_t col, uint32_t row)
{
	void *p = nullptr;
	REQUIRE(vkMapMemory(f.dev, f.buf_mem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS);
	Rgba out = static_cast<const Rgba *>(p)[row * kDim + col];
	vkUnmapMemory(f.dev, f.buf_mem);
	return out;
}

} // namespace

TEST_CASE("#1602 vk_native: the equirect2 shaders map screen rays onto the sphere")
{
	Fixture f;
	if (!setup(f)) {
		SKIP("no Vulkan device available");
	}

	// radius 0 is how the packer spells +INFINITY: no intersection, the ray
	// direction itself.
	draw(f, 0.0f, 2.0f * kPi);
	const Rgba above = pixel(f, kDim / 2, 3);
	const Rgba below = pixel(f, kDim / 2, kDim - 4);
	INFO("above centre -> " << above << " (expected " << kTop << ")");
	INFO("below centre -> " << below << " (expected " << kBottom << ")");

	// GATE 0, first and on its own.
	INFO("a read of " << kClear << " means the draw never covered the pixel");
	REQUIRE_FALSE(above == kClear);
	REQUIRE_FALSE(below == kClear);

	// The hue pair, both directions. Swapped colours mean the clip-space Y or
	// the ray basis is upside down — the Vulkan-specific half of this port.
	CHECK(above == kTop);
	CHECK(below == kBottom);
	CHECK_FALSE(above == below);

	// The spec's +INFINITY itself must pack to the same answer.
	draw(f, INFINITY, 2.0f * kPi);
	CHECK(pixel(f, kDim / 2, 3) == kTop);
	CHECK(pixel(f, kDim / 2, kDim - 4) == kBottom);

	// A FINITE radius with the camera at the centre: the intersection branch,
	// same answer.
	draw(f, 1.0f, 2.0f * kPi);
	const Rgba above_r = pixel(f, kDim / 2, 3);
	const Rgba below_r = pixel(f, kDim / 2, kDim - 4);
	INFO("finite radius: above -> " << above_r << ", below -> " << below_r);
	REQUIRE_FALSE(above_r == kClear);
	CHECK(above_r == kTop);
	CHECK(below_r == kBottom);
}

TEST_CASE("#1602 vk_native: an equirect2 layer covers only its angular extent, leaving the rest of the tile alone")
{
	Fixture f;
	if (!setup(f)) {
		SKIP("no Vulkan device available");
	}

	// 45° horizontal extent on a 90° frustum: the centre column is inside,
	// the outermost columns outside. Blending is OFF.
	draw(f, 0.0f, kPi / 4.0f);

	const uint32_t mid_row = kDim / 2;
	const Rgba inside = pixel(f, kDim / 2, mid_row);
	const Rgba left_edge = pixel(f, 0, mid_row);
	const Rgba right_edge = pixel(f, kDim - 1, mid_row);
	INFO("inside the extent -> " << inside);
	INFO("left  edge (outside) -> " << left_edge);
	INFO("right edge (outside) -> " << right_edge);

	REQUIRE_FALSE(inside == kClear);

	// A sub-rect layer only MARKS the tile. Outside its extent the clear must
	// survive, which under blending-off requires `discard`.
	INFO("outside the extent the tile must still hold the clear colour " << kClear);
	CHECK(left_edge == kClear);
	CHECK(right_edge == kClear);
}
