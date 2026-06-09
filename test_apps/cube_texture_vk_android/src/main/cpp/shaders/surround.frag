// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Faithful GLSL port of cube_texture_d3d11_win's g_surroundPSSource: a
// checkerboard + soft gradient over the window, a bright red border just
// outside the canvas hole (so the canvas/surround boundary is unmistakable),
// and a slow diagonal sweep. Pixels INSIDE the canvas are black — the app
// blits the runtime-woven canvas region over them.
#version 450

layout(push_constant) uniform SurroundParams {
    vec2  windowSize;   // present-image client area, px
    vec2  _pad0;
    ivec4 canvas;       // (x, y, w, h) in present-image px
    float time;         // seconds, for the sweep
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 p = gl_FragCoord.xy;
    if (p.x >= pc.windowSize.x || p.y >= pc.windowSize.y) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float bx0 = float(pc.canvas.x);
    float by0 = float(pc.canvas.y);
    float bx1 = float(pc.canvas.x + pc.canvas.z);
    float by1 = float(pc.canvas.y + pc.canvas.w);
    float dx = max(bx0 - p.x, p.x - bx1);
    float dy = max(by0 - p.y, p.y - by1);
    float d  = max(dx, dy);   // < 0 inside canvas, > 0 outside
    if (d <= 0.0) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    if (d <= 4.0) {
        outColor = vec4(1.0, 0.25, 0.25, 1.0);  // bright canvas border
        return;
    }
    ivec2 cell = ivec2(p / 24.0);
    bool light = ((cell.x + cell.y) & 1) == 0;
    vec3 base = light ? vec3(0.82, 0.84, 0.92) : vec3(0.50, 0.55, 0.80);
    vec2 g = clamp(p / pc.windowSize, 0.0, 1.0);
    base.r += g.x * 0.18;
    base.g += g.y * 0.10;
    base.b += (1.0 - g.x) * 0.10;
    float sweep = fract((p.x + p.y) / 256.0 - pc.time * 0.10);
    base += (smoothstep(0.45, 0.5, sweep) - smoothstep(0.5, 0.55, sweep)) * 0.10;
    outColor = vec4(clamp(base, 0.0, 1.0), 1.0);
}
