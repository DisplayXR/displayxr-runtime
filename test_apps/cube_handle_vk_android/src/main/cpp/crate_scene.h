// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// crate_scene — the Wood_Crate textured cube + floor grid, ported from
// cube_handle_vk_win's vk_renderer.cpp (#499). Self-contained Vulkan: two
// pipelines (textured cube w/ basecolor+normal+AO, line grid), indexed cube
// geometry, a generated grid, and three textures loaded from the APK assets.
//
// The host app (main.cpp) owns the OpenXR session, the tiled-atlas swapchain,
// the render pass, and the per-view rig/projection math; it computes column-
// major MVP + model matrices and calls the draw_* helpers inside its own render
// pass, once per tile.

#pragma once

#include <vulkan/vulkan.h>
#include <android/asset_manager.h>
#include <cstdint>

struct CrateScene
{
	VkDevice device{VK_NULL_HANDLE};
	VkPhysicalDevice phys{VK_NULL_HANDLE};
	VkQueue queue{VK_NULL_HANDLE};
	uint32_t queue_family{0};
	VkCommandPool upload_pool{VK_NULL_HANDLE};

	// Pipelines + layouts.
	VkDescriptorSetLayout desc_set_layout{VK_NULL_HANDLE};
	VkPipelineLayout cube_layout{VK_NULL_HANDLE};  // descriptor set + 128-byte push
	VkPipelineLayout grid_layout{VK_NULL_HANDLE};  // 80-byte push
	VkPipeline cube_pipeline{VK_NULL_HANDLE};
	VkPipeline grid_pipeline{VK_NULL_HANDLE};

	// Geometry.
	VkBuffer cube_vbuf{VK_NULL_HANDLE};
	VkDeviceMemory cube_vmem{VK_NULL_HANDLE};
	VkBuffer cube_ibuf{VK_NULL_HANDLE};
	VkDeviceMemory cube_imem{VK_NULL_HANDLE};
	VkBuffer grid_vbuf{VK_NULL_HANDLE};
	VkDeviceMemory grid_vmem{VK_NULL_HANDLE};
	uint32_t grid_vertex_count{0};

	// Textures (basecolor, normal, AO) + sampler + descriptor.
	VkImage tex_img[3]{};
	VkDeviceMemory tex_mem[3]{};
	VkImageView tex_view[3]{};
	VkSampler sampler{VK_NULL_HANDLE};
	VkDescriptorPool desc_pool{VK_NULL_HANDLE};
	VkDescriptorSet desc_set{VK_NULL_HANDLE};
	bool textures_loaded{false};
};

// Create pipelines, geometry, and textures (loaded from `assets`). `render_pass`
// is the host's atlas render pass the scene pipelines are compiled against.
bool
crate_scene_init(CrateScene &s, VkDevice device, VkPhysicalDevice phys, VkQueue queue,
                 uint32_t queue_family, VkRenderPass render_pass, AAssetManager *assets);

// Draw the textured cube. `mvp` and `model` are column-major float[16]; `model`
// transforms normals/tangents to world space for the lighting.
void
crate_scene_draw_cube(const CrateScene &s, VkCommandBuffer cmd, const float mvp[16],
                      const float model[16]);

// Draw the floor grid. `mvp` is column-major float[16]; `color` is RGBA.
void
crate_scene_draw_grid(const CrateScene &s, VkCommandBuffer cmd, const float mvp[16],
                      const float color[4]);

void
crate_scene_destroy(CrateScene &s);
