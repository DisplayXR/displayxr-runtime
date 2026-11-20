// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
//
// The weave-rect cursor of the Vulkan XR_DXR_weave engines (ADR-046 Amendment
// 6): a fullscreen triangle. The VIEWPORT is the sprite's (blur-padded) box in
// the atlas, so uv 0..1 spans that box, top-left origin.

#version 450

layout(location = 0) out vec2 out_uv;

void main()
{
	vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	out_uv = uv;
	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
