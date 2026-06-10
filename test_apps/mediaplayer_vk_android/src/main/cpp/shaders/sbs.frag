// SPDX-License-Identifier: BSL-1.0
// Sample one half of a side-by-side source and emit RGB. The source is either an RGBA
// texture (images: mode 0) or planar YUV from the video decoder (mode 1 = I420 three
// planes, mode 2 = NV12 two planes). For YUV the GPU does BOTH the colour convert and
// the downscale (the texture is the decoder's native frame; the viewport is the small
// per-eye tile), so the decode thread no longer runs swscale.
//
// uvOffset/uvScale select the SBS half: left eye = offset(0,0) scale(0.5,1), right eye =
// offset(0.5,0) scale(0.5,1); a 2D source uses offset(0,0) scale(1,1).
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D plane0;  // RGBA (mode 0) or Y (modes 1/2)
layout(binding = 1) uniform sampler2D plane1;  // U (I420) or interleaved UV (NV12)
layout(binding = 2) uniform sampler2D plane2;  // V (I420 only)

layout(push_constant) uniform PushConstants {
    vec2 uvOffset;
    vec2 uvScale;
    int mode;        // 0 = RGBA, 1 = I420, 2 = NV12
    float fullRange; // 1 = full/JPEG range, 0 = limited/MPEG range
} pc;

void main() {
    vec2 uv = pc.uvOffset + vUV * pc.uvScale;

    if (pc.mode == 0) {
        outColor = texture(plane0, uv);
        return;
    }

    float y = texture(plane0, uv).r;
    float u, v;
    if (pc.mode == 2) {            // NV12: chroma interleaved in .rg
        vec2 c = texture(plane1, uv).rg;
        u = c.r;
        v = c.g;
    } else {                       // I420: separate U and V planes
        u = texture(plane1, uv).r;
        v = texture(plane2, uv).r;
    }

    // Range-expand, then BT.709. Full range uses the samples directly; limited range
    // remaps Y [16,235]->[0,1] and chroma [16,240]->[-0.5,0.5] (8-bit).
    float yb, ub, vb;
    if (pc.fullRange > 0.5) {
        yb = y;
        ub = u - 0.5;
        vb = v - 0.5;
    } else {
        yb = (y - 16.0 / 255.0) * (255.0 / 219.0);
        ub = (u - 128.0 / 255.0) * (255.0 / 224.0);
        vb = (v - 128.0 / 255.0) * (255.0 / 224.0);
    }
    vec3 rgb = vec3(yb + 1.5748 * vb,
                    yb - 0.1873 * ub - 0.4681 * vb,
                    yb + 1.8556 * ub);
    outColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
