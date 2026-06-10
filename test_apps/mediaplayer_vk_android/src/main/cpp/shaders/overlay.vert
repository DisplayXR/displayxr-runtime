// SPDX-License-Identifier: BSL-1.0
// 2D transport-overlay geometry. Positions arrive already in clip space (NDC),
// built CPU-side from transport_ui.h fractions; per-vertex RGBA is passed
// straight through for solid/translucent fills. Drawn after the SBS blit, in
// the same render pass, identically in both eyes (zero disparity = screen plane).
#version 450

layout(location = 0) in vec2 inPos;    // NDC
layout(location = 1) in vec4 inColor;  // straight RGBA

layout(location = 0) out vec4 vColor;

void main() {
    vColor = inColor;
    gl_Position = vec4(inPos, 0.0, 1.0);
}
