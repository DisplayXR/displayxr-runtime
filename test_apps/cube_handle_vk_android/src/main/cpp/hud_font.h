// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// hud_font — crisp antialiased HUD text for cube_handle_vk_android (#499).
//
// The Windows app gets sharp text from DirectWrite and macOS from CoreText;
// neither is portable to Android. Here we bake a real TTF (a /system/fonts
// face) into an alpha atlas with stb_truetype, then draw textured glyph quads
// that sample the atlas as coverage — same crispness, no platform font API.
//
// Self-contained Vulkan: its own R8 atlas texture + sampler + descriptor set +
// textured pipeline (alpha-blended, no depth) + a mapped quad vertex buffer.
// Render order in the frame: draw the translucent panel (app's solid pipeline),
// then hud_font_draw() the text on top.

#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

#define HUD_FONT_FIRST_CHAR 32
#define HUD_FONT_NUM_CHARS 96      // ASCII 32..127
#define HUD_FONT_MAX_QUADS 256     // plenty for a few short lines

struct HudFontBakedChar {          // mirrors stbtt_bakedchar (avoids leaking stb in the header)
	uint16_t x0, y0, x1, y1;
	float xoff, yoff, xadvance;
};

struct HudFont {
	bool ready = false;

	VkDevice device = VK_NULL_HANDLE;

	// Glyph atlas (R8 coverage) baked by stb_truetype.
	VkImage atlas_img = VK_NULL_HANDLE;
	VkDeviceMemory atlas_mem = VK_NULL_HANDLE;
	VkImageView atlas_view = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	uint32_t atlas_w = 0, atlas_h = 0;
	float pixel_height = 0.0f;
	HudFontBakedChar chars[HUD_FONT_NUM_CHARS] = {};

	// Descriptor + pipeline.
	VkDescriptorSetLayout dset_layout = VK_NULL_HANDLE;
	VkDescriptorPool dpool = VK_NULL_HANDLE;
	VkDescriptorSet dset = VK_NULL_HANDLE;
	VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;

	// Persistently-mapped quad vertex buffer ({vec2 pos; vec2 uv}).
	VkBuffer vbuf = VK_NULL_HANDLE;
	VkDeviceMemory vbuf_mem = VK_NULL_HANDLE;
	void *vbuf_mapped = nullptr;
};

// Bake a system TTF + create all GPU resources. `pixel_height` is the font
// raster size (≈48 is crisp for a HUD). Returns false (and leaves ready=false)
// if the font can't be loaded or any resource fails — the caller falls back to
// the legacy bitmap HUD.
bool hud_font_init(HudFont &f, VkPhysicalDevice phys, VkDevice device, VkQueue queue,
                   uint32_t queue_family, VkRenderPass rp, float pixel_height);

// Build textured quads for `text` (supports '\n') into the mapped vertex buffer.
//   ox, oy      — top-left of the text block, in HUD NDC (pre-vflip).
//   ndc_per_px  — NDC units per font pixel (controls on-screen size).
//   line_step   — NDC advance between lines.
// Returns the vertex count to pass to hud_font_draw().
uint32_t hud_font_build(HudFont &f, const char *text, float ox, float oy, float ndc_per_px,
                        float line_step);

// Draw the built quads. mvp = the HUD MVP (the v-flip the app already uses);
// color = RGBA (alpha multiplies the glyph coverage).
void hud_font_draw(HudFont &f, VkCommandBuffer cmd, const float mvp[16], const float color[4],
                   uint32_t vert_count);

void hud_font_destroy(HudFont &f);
