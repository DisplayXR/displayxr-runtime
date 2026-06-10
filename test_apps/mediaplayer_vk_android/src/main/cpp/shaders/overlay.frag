// SPDX-License-Identifier: BSL-1.0
// Transport-overlay fragment: emit the interpolated per-vertex RGBA. Alpha
// blending is configured on the pipeline so translucent panels composite over
// the video.
#version 450

layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vColor;
}
