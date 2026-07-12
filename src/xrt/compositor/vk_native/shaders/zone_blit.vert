// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
//
// XR_DXR_display_zones (ADR-027): fullscreen-triangle vertex shader for the
// zone alpha-over draw path. The viewport is set to the zone's destination
// rect inside the atlas tile; the push-constant src_rect maps the triangle's
// 0..1 uv onto the zone's view-tile sub-rect of the source swapchain.

#version 450

layout(push_constant) uniform ZoneParams {
	vec4 src_rect; // x, y, w, h — normalized source-texture coordinates
} pc;

layout(location = 0) out vec2 out_uv;

void main()
{
	// Fullscreen triangle: 3 vertices, no VBO.
	vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
	out_uv = pc.src_rect.xy + uv * pc.src_rect.zw;
}
