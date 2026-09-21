// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
//
// LAYERED (arraySize > 1) twin of zone_blit.frag.
//
// Under single-pass-instanced stereo an app submits ONE swapchain with
// arraySize = 2 and addresses each view by subImage.imageArrayIndex
// (ADR-032). Its VkImageView is then a VK_IMAGE_VIEW_TYPE_2D_ARRAY, which a
// `sampler2D` cannot bind — so before this variant existed the draw pass
// refused the WHOLE FRAME and fell back to vkCmdBlitImage (which cannot
// blend). Same split as D3D11's projection_ps_array_source and GL's
// FS_BLIT_ARRAY.
//
// The slice rides the push block rather than a specialization constant
// because it changes per draw, not per pipeline.

#version 450

layout(binding = 0) uniform sampler2DArray src_tex;

layout(push_constant) uniform ZoneParams {
	vec4 src_rect; // x, y, w, h — normalized source-texture coordinates
	vec4 params;   // x = array slice (subImage.imageArrayIndex); yzw reserved
} pc;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	out_color = texture(src_tex, vec3(in_uv, pc.params.x));
}
