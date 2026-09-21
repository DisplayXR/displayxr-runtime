// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
//
// Compose-pass fragment shader (sampler2D source).
//
// Ends in `color * color_scale + color_bias` — the
// XR_KHR_composition_layer_color_scale_bias channel every layer shader in the
// tree applies, identity when the app asked for nothing. That one line is
// also how COMP_LAYER_BLEND_OPAQUE_COVER gets its spec-mandated alpha of one:
// comp_layer_blend_fold_opaque_cover() sets scale.a = 0, bias.a = 1 on the
// CPU side. Fixed-function blending cannot synthesise a constant 1 from an
// arbitrary src.a, and folding it here costs no extra push-constant field and
// no second shader variant.

#version 450

layout(binding = 0) uniform sampler2D src_tex;

layout(push_constant) uniform ComposeParams {
	mat4 mvp;
	vec4 src_rect;
	vec4 params;      // x = array slice; y = quad flag
	vec4 color_scale;
	vec4 color_bias;
} pc;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	out_color = texture(src_tex, in_uv) * pc.color_scale + pc.color_bias;
}
