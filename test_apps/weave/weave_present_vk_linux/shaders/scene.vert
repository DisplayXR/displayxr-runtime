// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// Full-target triangle; the fragment shader does all the work.
// Regenerate the embedded SPIR-V with ./compile.sh after editing.
#version 450
void main()
{
	vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
