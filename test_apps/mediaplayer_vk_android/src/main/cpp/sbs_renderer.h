// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// SbsRenderer — minimal side-by-side blit for the Android media-player port of
// displayxr-demo-mediaplayer. Adapts David's src/rhi/VulkanRenderer (the SBS
// split + sbs.frag/fullscreen.vert pipeline) to this repo's OpenXR-Android
// harness, which renders into TWO per-view swapchains (one image per eye) rather
// than a single SBS image. So drawEye() blits one UV half of the source texture,
// full-viewport, into the eye's swapchain image; the runtime's Leia DP weaves
// the two eyes.
//
// Stage 1 (image MVP): mode 0 (RGBA) only — an SBS PNG decoded via stb. The
// shader already carries mode 1/2 (I420/NV12) for the Stage-2 AMediaCodec video
// path; uploadYUV will land then.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <unordered_map>

struct SbsRenderer {
	// Borrow the Vulkan device/queue the OpenXR runtime selected. format =
	// the per-view swapchain image format. Builds the blit pipeline, sampler,
	// descriptor set, command buffer + fence.
	bool init(VkPhysicalDevice phys, VkDevice device, VkQueue queue,
	          uint32_t queueFamily, VkFormat format);

	// Upload an RGBA8 image as the SBS source (the whole stereo pair). Replaces
	// any previous source. Safe to call between frames.
	bool uploadTexture(const uint8_t *rgba, uint32_t width, uint32_t height);

	bool hasTexture() const { return texView_ != VK_NULL_HANDLE; }

	// Blit the UV sub-rect [off, off+scale) of the source into `image` (a
	// per-view swapchain image of size w x h), clearing to black first. Left
	// eye = off(0,0) scale(0.5,1); right eye = off(0.5,0) scale(0.5,1). Blocks
	// until the GPU finishes (the runtime samples right after release).
	void drawEye(VkImage image, uint32_t w, uint32_t h, float offX, float offY,
	             float scaleX, float scaleY);

	void cleanup();

private:
	struct Target {  // cached per swapchain image
		VkImageView view = VK_NULL_HANDLE;
		VkFramebuffer fb = VK_NULL_HANDLE;
		uint32_t w = 0, h = 0;
	};
	const Target &targetFor(VkImage image, uint32_t w, uint32_t h);
	uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
	void bindDescriptor();
	void destroyTexture();

	VkPhysicalDevice phys_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	VkQueue queue_ = VK_NULL_HANDLE;
	uint32_t queueFamily_ = 0;
	VkFormat format_ = VK_FORMAT_UNDEFINED;

	VkRenderPass renderPass_ = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
	VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
	VkPipeline pipeline_ = VK_NULL_HANDLE;
	VkSampler sampler_ = VK_NULL_HANDLE;
	VkDescriptorPool descPool_ = VK_NULL_HANDLE;
	VkDescriptorSet descSet_ = VK_NULL_HANDLE;
	VkCommandPool cmdPool_ = VK_NULL_HANDLE;
	VkCommandBuffer cmd_ = VK_NULL_HANDLE;
	VkFence fence_ = VK_NULL_HANDLE;

	// Source texture (mode 0 RGBA). Bound to all three sampler slots so the
	// shader's unused YUV bindings stay valid in mode 0.
	VkImage texImage_ = VK_NULL_HANDLE;
	VkDeviceMemory texMem_ = VK_NULL_HANDLE;
	VkImageView texView_ = VK_NULL_HANDLE;

	std::unordered_map<VkImage, Target> targets_;
};
