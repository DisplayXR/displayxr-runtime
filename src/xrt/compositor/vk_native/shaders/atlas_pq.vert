// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
//
// #2340 HDR10 - the fullscreen triangle for the atlas PQ re-encode pass
// (atlas_pq.frag). No inputs: the fragment shader addresses its source by
// gl_FragCoord, so the triangle only has to cover the viewport.

#version 450

void main()
{
	vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
