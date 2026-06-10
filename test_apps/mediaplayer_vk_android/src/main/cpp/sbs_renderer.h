// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// SbsRenderer — side-by-side blit for the Android media-player port of
// displayxr-demo-mediaplayer. Adapts David's src/rhi/VulkanRenderer (the SBS
// split + sbs.frag/fullscreen.vert pipeline) to this repo's OpenXR-Android
// harness, which renders into TWO per-view swapchains (one image per eye)
// rather than a single SBS image. drawEye() blits one UV half of the source,
// full-viewport, into the eye's swapchain image; the runtime's Leia DP weaves.
//
//   - uploadTexture(): RGBA8 source (mode 0) — SBS still images via stb.
//   - uploadYUV():     planar Y + chroma (mode 1 I420 / mode 2 NV12) — the
//                      AMediaCodec video path; the GPU does the BT.709 convert.
//
// Planes are reused frame-to-frame (reallocated only on a size/format change)
// with a persistent host-visible staging buffer each, so per-frame video upload
// is a memcpy + one transfer submit — no per-frame Vulkan object churn.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <unordered_map>

struct SbsRenderer {
	bool init(VkPhysicalDevice phys, VkDevice device, VkQueue queue,
	          uint32_t queueFamily, VkFormat format);

	// RGBA8 still image (mode 0). Replaces the current source.
	bool uploadTexture(const uint8_t *rgba, uint32_t width, uint32_t height);

	// Planar YUV video frame (the decoder's native output). nv12: plane1 =
	// interleaved UV (w/2 x h/2, 2 bytes/texel), plane2 unused. !nv12 (I420):
	// plane1 = U, plane2 = V (each w/2 x h/2). Planes are tightly packed.
	bool uploadYUV(const uint8_t *y, const uint8_t *uv_or_u, const uint8_t *v,
	               uint32_t width, uint32_t height, bool nv12, bool fullRange);

	bool hasSource() const { return planes_[0].view != VK_NULL_HANDLE; }

	// Transport overlay (scrub bar + play/pause + load button + time), drawn
	// into each eye at zero disparity (screen plane). progress in [0,1]. left/
	// right are short time strings (e.g. "0:42"). Layout: transport_ui.h.
	void setOverlay(bool visible, float progress, bool paused, const char *left,
	                const char *right);

	// Blit the UV sub-rect [off, off+scale) into `image` (a per-view swapchain
	// image of size w x h), clearing to black first. Left eye = off(0,0)
	// scale(0.5,1); right eye = off(0.5,0). Blocks until the GPU finishes.
	void drawEye(VkImage image, uint32_t w, uint32_t h, float offX, float offY,
	             float scaleX, float scaleY);

	void cleanup();

private:
	struct Plane {
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		uint32_t w = 0, h = 0;
		VkFormat fmt = VK_FORMAT_UNDEFINED;
		bool initialized = false;  // false right after (re)create → barrier from UNDEFINED
		VkBuffer staging = VK_NULL_HANDLE;
		VkDeviceMemory stagingMem = VK_NULL_HANDLE;
		VkDeviceSize stagingSize = 0;
		void *mapped = nullptr;
	};
	struct Target {  // cached per swapchain image
		VkImageView view = VK_NULL_HANDLE;
		VkFramebuffer fb = VK_NULL_HANDLE;
		uint32_t w = 0, h = 0;
	};

	const Target &targetFor(VkImage image, uint32_t w, uint32_t h);
	bool initOverlayPipeline();
	void drawOverlay(VkCommandBuffer cmd, uint32_t w, uint32_t h);
	uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
	bool ensurePlane(int idx, uint32_t w, uint32_t h, VkFormat fmt, uint32_t bytesPerTexel);
	void recordPlaneCopy(VkCommandBuffer cmd, int idx, const uint8_t *src, uint32_t w,
	                     uint32_t h, uint32_t bytesPerTexel);
	bool ensureDummy();
	void bindDescriptors();  // point the 3 sampler slots at planes_/dummy
	void destroyPlane(Plane &p);

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

	Plane planes_[3];
	int sourceMode_ = 0;          // 0 RGBA, 1 I420, 2 NV12 (pushed to the shader)
	float sourceFullRange_ = 1.0f;
	VkImage dummyImage_ = VK_NULL_HANDLE;
	VkDeviceMemory dummyMemory_ = VK_NULL_HANDLE;
	VkImageView dummyView_ = VK_NULL_HANDLE;

	std::unordered_map<VkImage, Target> targets_;

	// 2D triangle overlay pipeline (transport bar geometry). Vertices are
	// (vec2 NDC, vec4 RGBA); alpha-blended over the video. The vertex buffer is
	// host-visible and rebuilt each drawOverlay (serialized by drawEye's
	// per-eye fence wait, so the prior submit is done before we overwrite).
	VkPipelineLayout ovPipeLayout_ = VK_NULL_HANDLE;
	VkPipeline ovPipeline_ = VK_NULL_HANDLE;
	VkBuffer ovVbo_ = VK_NULL_HANDLE;
	VkDeviceMemory ovVboMem_ = VK_NULL_HANDLE;
	void *ovVboMapped_ = nullptr;
	uint32_t ovVboCapVerts_ = 0;

	// Transport overlay state (set per frame from main).
	bool ovVisible_ = false;
	float ovProgress_ = 0.0f;
	bool ovPaused_ = false;
	char ovLeft_[12] = {0};
	char ovRight_[12] = {0};
};
