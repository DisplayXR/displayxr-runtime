// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
//
// One vertex shader for every layer the compose pass draws.
//
//  - params.y == 0 — FULLSCREEN: the 3-vertex triangle the projection and
//    zone draws have always used. The destination rect is the viewport; the
//    push-constant src_rect maps the triangle's 0..1 uv onto the source
//    sub-rect. `mvp` is ignored, so those draws are bit-identical to before
//    quads existed.
//  - params.y != 0 — QUAD (#1581): a world-placed 1x1 quad centred on the
//    origin, scaled by the layer size in the model matrix, drawn as two
//    triangles (6 vertices) from a 4-corner table.
//
// ONE Y negation here, and it is NOT the two Metal needs. `pos.y = -pos.y`
// (quad-local, before the model matrix) pairs the texture's TOP row with the
// quad's TOP edge in OpenXR's +Y-up quad space — the same flip
// shaders/layer_quad.vert has. Metal additionally flips clip Y because its
// NDC +y is the TOP of the framebuffer; Vulkan's NDC +y is the BOTTOM, which
// is precisely the convention math_matrix_4x4_projection_vulkan_infinite_reverse
// is built for, so there is NO second flip on this backend.
//
// Do not "unify" that with the Metal shader by copying its clip flip across:
// the pair (projection helper, clip flip) has to change together or not at
// all. Vulkan helper + clip flip would mirror every quad about the tile's
// horizontal centre line — silently, because a quad near the view axis barely
// moves.

#version 450

layout(push_constant) uniform ComposeParams {
	mat4 mvp;         // quad only; ignored on the fullscreen path
	vec4 src_rect;    // x, y, w, h — normalized source-texture coordinates
	vec4 params;      // x = array slice (fragment), y = quad flag; zw reserved
	vec4 color_scale; // XR_KHR_composition_layer_color_scale_bias (fragment)
	vec4 color_bias;  // ditto
} pc;

layout(location = 0) out vec2 out_uv;

// Two triangles over the unit square, as vertex ids 0..5.
const vec2 QUAD_CORNERS[6] = vec2[6](
	vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 0.0),
	vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0)
);

void main()
{
	if (pc.params.y < 0.5) {
		// Fullscreen triangle: 3 vertices, no VBO. Unchanged.
		vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
		gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
		out_uv = pc.src_rect.xy + uv * pc.src_rect.zw;
		return;
	}

	vec2 uv = QUAD_CORNERS[gl_VertexIndex % 6];
	vec2 pos = uv - 0.5;
	pos.y = -pos.y; // texture top <-> quad top (+Y up)
	gl_Position = pc.mvp * vec4(pos, 0.0, 1.0);
	out_uv = pc.src_rect.xy + uv * pc.src_rect.zw;
}
