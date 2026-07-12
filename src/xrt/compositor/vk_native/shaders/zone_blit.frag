// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
//
// XR_DXR_display_zones (ADR-027): sample the zone's source view tile.
// Alpha-over compositing (premultiplied or straight) is pipeline blend
// state — two pipeline variants share this shader pair.

#version 450

layout(binding = 0) uniform sampler2D src_tex;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	out_color = texture(src_tex, in_uv);
}
