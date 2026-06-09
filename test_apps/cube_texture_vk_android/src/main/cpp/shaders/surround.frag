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
    // Checkerboard surround removed — plain solid black so only the textured
    // cube (woven into the canvas, blitted over this) shows. canvas/windowSize
    // kept in the push-constant layout for parity with the C++ side.
    outColor = vec4(0.0, 0.0, 0.0, 1.0);
}
