// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan rendering implementation for cube and grid
 */

#include "vk_renderer.h"
#include "logging.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <string>
#include <cmath>
#include <vector>
#include <cstring>

using namespace DirectX;

// ============================================================================
// Embedded SPIR-V shaders
// ============================================================================
// Textured cube vertex shader: takes position + color + uv + normal + tangent,
// applies MVP transform, outputs world-space normal/tangent via model matrix
static const uint32_t g_cubeTexturedVertSpv[] = {
    0x07230203,0x00010000,0x0008000b,0x00000043,0x00000000,0x00020011,
    0x00000001,0x0006000b,0x00000001,0x4c534c47,0x6474732e,0x3035342e,
    0x00000000,0x0003000e,0x00000000,0x00000001,0x000e000f,0x00000000,
    0x00000004,0x6e69616d,0x00000000,0x0000000d,0x00000019,0x00000025,
    0x00000027,0x00000037,0x00000039,0x0000003c,0x0000003e,0x00000042,
    0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,
    0x00000000,0x00060005,0x0000000b,0x505f6c67,0x65567265,0x78657472,
    0x00000000,0x00060006,0x0000000b,0x00000000,0x505f6c67,0x7469736f,
    0x006e6f69,0x00070006,0x0000000b,0x00000001,0x505f6c67,0x746e696f,
    0x657a6953,0x00000000,0x00070006,0x0000000b,0x00000002,0x435f6c67,
    0x4470696c,0x61747369,0x0065636e,0x00070006,0x0000000b,0x00000003,
    0x435f6c67,0x446c6c75,0x61747369,0x0065636e,0x00030005,0x0000000d,
    0x00000000,0x00060005,0x00000011,0x68737550,0x736e6f43,0x746e6174,
    0x00000073,0x00040006,0x00000011,0x00000000,0x0070766d,0x00050006,
    0x00000011,0x00000001,0x65646f6d,0x0000006c,0x00030005,0x00000013,
    0x00000000,0x00050005,0x00000019,0x6f506e69,0x69746973,0x00006e6f,
    0x00040005,0x00000025,0x67617266,0x00005655,0x00040005,0x00000027,
    0x56556e69,0x00000000,0x00050005,0x0000002b,0x6d726f6e,0x614d6c61,
    0x00000074,0x00060005,0x00000037,0x67617266,0x6c726f57,0x726f4e64,
    0x006c616d,0x00050005,0x00000039,0x6f4e6e69,0x6c616d72,0x00000000,
    0x00070005,0x0000003c,0x67617266,0x6c726f57,0x6e615464,0x746e6567,
    0x00000000,0x00050005,0x0000003e,0x61546e69,0x6e65676e,0x00000074,
    0x00040005,0x00000042,0x6f436e69,0x00726f6c,0x00030047,0x0000000b,
    0x00000002,0x00050048,0x0000000b,0x00000000,0x0000000b,0x00000000,
    0x00050048,0x0000000b,0x00000001,0x0000000b,0x00000001,0x00050048,
    0x0000000b,0x00000002,0x0000000b,0x00000003,0x00050048,0x0000000b,
    0x00000003,0x0000000b,0x00000004,0x00030047,0x00000011,0x00000002,
    0x00040048,0x00000011,0x00000000,0x00000005,0x00050048,0x00000011,
    0x00000000,0x00000007,0x00000010,0x00050048,0x00000011,0x00000000,
    0x00000023,0x00000000,0x00040048,0x00000011,0x00000001,0x00000005,
    0x00050048,0x00000011,0x00000001,0x00000007,0x00000010,0x00050048,
    0x00000011,0x00000001,0x00000023,0x00000040,0x00040047,0x00000019,
    0x0000001e,0x00000000,0x00040047,0x00000025,0x0000001e,0x00000000,
    0x00040047,0x00000027,0x0000001e,0x00000002,0x00040047,0x00000037,
    0x0000001e,0x00000001,0x00040047,0x00000039,0x0000001e,0x00000003,
    0x00040047,0x0000003c,0x0000001e,0x00000002,0x00040047,0x0000003e,
    0x0000001e,0x00000004,0x00040047,0x00000042,0x0000001e,0x00000001,
    0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,
    0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,
    0x00040015,0x00000008,0x00000020,0x00000000,0x0004002b,0x00000008,
    0x00000009,0x00000001,0x0004001c,0x0000000a,0x00000006,0x00000009,
    0x0006001e,0x0000000b,0x00000007,0x00000006,0x0000000a,0x0000000a,
    0x00040020,0x0000000c,0x00000003,0x0000000b,0x0004003b,0x0000000c,
    0x0000000d,0x00000003,0x00040015,0x0000000e,0x00000020,0x00000001,
    0x0004002b,0x0000000e,0x0000000f,0x00000000,0x00040018,0x00000010,
    0x00000007,0x00000004,0x0004001e,0x00000011,0x00000010,0x00000010,
    0x00040020,0x00000012,0x00000009,0x00000011,0x0004003b,0x00000012,
    0x00000013,0x00000009,0x00040020,0x00000014,0x00000009,0x00000010,
    0x00040017,0x00000017,0x00000006,0x00000003,0x00040020,0x00000018,
    0x00000001,0x00000017,0x0004003b,0x00000018,0x00000019,0x00000001,
    0x0004002b,0x00000006,0x0000001b,0x3f800000,0x00040020,0x00000021,
    0x00000003,0x00000007,0x00040017,0x00000023,0x00000006,0x00000002,
    0x00040020,0x00000024,0x00000003,0x00000023,0x0004003b,0x00000024,
    0x00000025,0x00000003,0x00040020,0x00000026,0x00000001,0x00000023,
    0x0004003b,0x00000026,0x00000027,0x00000001,0x00040018,0x00000029,
    0x00000017,0x00000003,0x00040020,0x0000002a,0x00000007,0x00000029,
    0x0004002b,0x0000000e,0x0000002c,0x00000001,0x00040020,0x00000036,
    0x00000003,0x00000017,0x0004003b,0x00000036,0x00000037,0x00000003,
    0x0004003b,0x00000018,0x00000039,0x00000001,0x0004003b,0x00000036,
    0x0000003c,0x00000003,0x0004003b,0x00000018,0x0000003e,0x00000001,
    0x00040020,0x00000041,0x00000001,0x00000007,0x0004003b,0x00000041,
    0x00000042,0x00000001,0x00050036,0x00000002,0x00000004,0x00000000,
    0x00000003,0x000200f8,0x00000005,0x0004003b,0x0000002a,0x0000002b,
    0x00000007,0x00050041,0x00000014,0x00000015,0x00000013,0x0000000f,
    0x0004003d,0x00000010,0x00000016,0x00000015,0x0004003d,0x00000017,
    0x0000001a,0x00000019,0x00050051,0x00000006,0x0000001c,0x0000001a,
    0x00000000,0x00050051,0x00000006,0x0000001d,0x0000001a,0x00000001,
    0x00050051,0x00000006,0x0000001e,0x0000001a,0x00000002,0x00070050,
    0x00000007,0x0000001f,0x0000001c,0x0000001d,0x0000001e,0x0000001b,
    0x00050091,0x00000007,0x00000020,0x00000016,0x0000001f,0x00050041,
    0x00000021,0x00000022,0x0000000d,0x0000000f,0x0003003e,0x00000022,
    0x00000020,0x0004003d,0x00000023,0x00000028,0x00000027,0x0003003e,
    0x00000025,0x00000028,0x00050041,0x00000014,0x0000002d,0x00000013,
    0x0000002c,0x0004003d,0x00000010,0x0000002e,0x0000002d,0x00050051,
    0x00000007,0x0000002f,0x0000002e,0x00000000,0x0008004f,0x00000017,
    0x00000030,0x0000002f,0x0000002f,0x00000000,0x00000001,0x00000002,
    0x00050051,0x00000007,0x00000031,0x0000002e,0x00000001,0x0008004f,
    0x00000017,0x00000032,0x00000031,0x00000031,0x00000000,0x00000001,
    0x00000002,0x00050051,0x00000007,0x00000033,0x0000002e,0x00000002,
    0x0008004f,0x00000017,0x00000034,0x00000033,0x00000033,0x00000000,
    0x00000001,0x00000002,0x00060050,0x00000029,0x00000035,0x00000030,
    0x00000032,0x00000034,0x0003003e,0x0000002b,0x00000035,0x0004003d,
    0x00000029,0x00000038,0x0000002b,0x0004003d,0x00000017,0x0000003a,
    0x00000039,0x00050091,0x00000017,0x0000003b,0x00000038,0x0000003a,
    0x0003003e,0x00000037,0x0000003b,0x0004003d,0x00000029,0x0000003d,
    0x0000002b,0x0004003d,0x00000017,0x0000003f,0x0000003e,0x00050091,
    0x00000017,0x00000040,0x0000003d,0x0000003f,0x0003003e,0x0000003c,
    0x00000040,0x000100fd,0x00010038,
};

// Textured cube fragment shader: lighting matches cube_handle_gl_win /
// cube_handle_metal_macos / cube_handle_gl_macos.
//
// GLSL source (regenerate via `glslangValidator -V cube.frag -o cube_frag.spv`):
//   #version 450
//   layout(set = 0, binding = 0) uniform sampler2D uBasecolorTex;
//   layout(set = 0, binding = 1) uniform sampler2D uNormalTex;
//   layout(set = 0, binding = 2) uniform sampler2D uAOTex;
//   layout(location = 0) in vec2 fragUV;
//   layout(location = 1) in vec3 fragWorldNormal;
//   layout(location = 2) in vec3 fragWorldTangent;
//   layout(location = 0) out vec4 outColor;
//   void main() {
//       vec3 baseColor = texture(uBasecolorTex, fragUV).xyz;
//       vec3 normalMap = texture(uNormalTex,    fragUV).xyz * 2.0 - 1.0;
//       float ao       = texture(uAOTex,        fragUV).x;
//       vec3 N = normalize(fragWorldNormal);
//       vec3 T = normalize(fragWorldTangent);
//       mat3 TBN = mat3(T, cross(N, T), N);
//       vec3 normal = normalize(TBN * normalMap);
//       vec3 lightDir = normalize(vec3(0.3, 0.5, 1.0));
//       float diffuse = max(dot(normal, lightDir), 0.0) * 0.8 * ao;
//       float ambient = 0.3 + 0.15 * ao;
//       outColor = vec4(baseColor * (diffuse + ambient), 1.0);
//   }
static const uint32_t g_cubeTexturedFragSpv[] = {
    0x07230203,0x00010000,0x0008000b,0x0000006f,0x00000000,0x00020011,
    0x00000001,0x0006000b,0x00000001,0x4c534c47,0x6474732e,0x3035342e,
    0x00000000,0x0003000e,0x00000000,0x00000001,0x0009000f,0x00000004,
    0x00000004,0x6e69616d,0x00000000,0x00000011,0x0000002c,0x00000030,
    0x00000065,0x00030010,0x00000004,0x00000007,0x00030003,0x00000002,
    0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00050005,
    0x00000009,0x65736162,0x6f6c6f43,0x00000072,0x00060005,0x0000000d,
    0x73614275,0x6c6f6365,0x6554726f,0x00000078,0x00040005,0x00000011,
    0x67617266,0x00005655,0x00050005,0x00000016,0x6d726f6e,0x614d6c61,
    0x00000070,0x00050005,0x00000017,0x726f4e75,0x546c616d,0x00007865,
    0x00030005,0x00000022,0x00006f61,0x00040005,0x00000023,0x544f4175,
    0x00007865,0x00030005,0x0000002a,0x0000004e,0x00060005,0x0000002c,
    0x67617266,0x6c726f57,0x726f4e64,0x006c616d,0x00030005,0x0000002f,
    0x00000054,0x00070005,0x00000030,0x67617266,0x6c726f57,0x6e615464,
    0x746e6567,0x00000000,0x00030005,0x00000033,0x00000042,0x00030005,
    0x00000039,0x004e4254,0x00040005,0x0000004b,0x6d726f6e,0x00006c61,
    0x00050005,0x00000050,0x6867696c,0x72694474,0x00000000,0x00040005,
    0x00000055,0x66666964,0x00657375,0x00040005,0x0000005e,0x69626d61,
    0x00746e65,0x00050005,0x00000065,0x4374756f,0x726f6c6f,0x00000000,
    0x00040047,0x0000000d,0x00000021,0x00000000,0x00040047,0x0000000d,
    0x00000022,0x00000000,0x00040047,0x00000011,0x0000001e,0x00000000,
    0x00040047,0x00000017,0x00000021,0x00000001,0x00040047,0x00000017,
    0x00000022,0x00000000,0x00040047,0x00000023,0x00000021,0x00000002,
    0x00040047,0x00000023,0x00000022,0x00000000,0x00040047,0x0000002c,
    0x0000001e,0x00000001,0x00040047,0x00000030,0x0000001e,0x00000002,
    0x00040047,0x00000065,0x0000001e,0x00000000,0x00020013,0x00000002,
    0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,
    0x00040017,0x00000007,0x00000006,0x00000003,0x00040020,0x00000008,
    0x00000007,0x00000007,0x00090019,0x0000000a,0x00000006,0x00000001,
    0x00000000,0x00000000,0x00000000,0x00000001,0x00000000,0x0003001b,
    0x0000000b,0x0000000a,0x00040020,0x0000000c,0x00000000,0x0000000b,
    0x0004003b,0x0000000c,0x0000000d,0x00000000,0x00040017,0x0000000f,
    0x00000006,0x00000002,0x00040020,0x00000010,0x00000001,0x0000000f,
    0x0004003b,0x00000010,0x00000011,0x00000001,0x00040017,0x00000013,
    0x00000006,0x00000004,0x0004003b,0x0000000c,0x00000017,0x00000000,
    0x0004002b,0x00000006,0x0000001c,0x40000000,0x0004002b,0x00000006,
    0x0000001e,0x3f800000,0x00040020,0x00000021,0x00000007,0x00000006,
    0x0004003b,0x0000000c,0x00000023,0x00000000,0x00040015,0x00000027,
    0x00000020,0x00000000,0x0004002b,0x00000027,0x00000028,0x00000000,
    0x00040020,0x0000002b,0x00000001,0x00000007,0x0004003b,0x0000002b,
    0x0000002c,0x00000001,0x0004003b,0x0000002b,0x00000030,0x00000001,
    0x00040018,0x00000037,0x00000007,0x00000003,0x00040020,0x00000038,
    0x00000007,0x00000037,0x0004002b,0x00000006,0x0000003d,0x00000000,
    0x0004002b,0x00000006,0x00000051,0x3e84b0b0,0x0004002b,0x00000006,
    0x00000052,0x3edd267b,0x0004002b,0x00000006,0x00000053,0x3f5d267b,
    0x0006002c,0x00000007,0x00000054,0x00000051,0x00000052,0x00000053,
    0x0004002b,0x00000006,0x0000005a,0x3f4ccccd,0x0004002b,0x00000006,
    0x0000005f,0x3e99999a,0x0004002b,0x00000006,0x00000060,0x3e19999a,
    0x00040020,0x00000064,0x00000003,0x00000013,0x0004003b,0x00000064,
    0x00000065,0x00000003,0x00050036,0x00000002,0x00000004,0x00000000,
    0x00000003,0x000200f8,0x00000005,0x0004003b,0x00000008,0x00000009,
    0x00000007,0x0004003b,0x00000008,0x00000016,0x00000007,0x0004003b,
    0x00000021,0x00000022,0x00000007,0x0004003b,0x00000008,0x0000002a,
    0x00000007,0x0004003b,0x00000008,0x0000002f,0x00000007,0x0004003b,
    0x00000008,0x00000033,0x00000007,0x0004003b,0x00000038,0x00000039,
    0x00000007,0x0004003b,0x00000008,0x0000004b,0x00000007,0x0004003b,
    0x00000008,0x00000050,0x00000007,0x0004003b,0x00000021,0x00000055,
    0x00000007,0x0004003b,0x00000021,0x0000005e,0x00000007,0x0004003d,
    0x0000000b,0x0000000e,0x0000000d,0x0004003d,0x0000000f,0x00000012,
    0x00000011,0x00050057,0x00000013,0x00000014,0x0000000e,0x00000012,
    0x0008004f,0x00000007,0x00000015,0x00000014,0x00000014,0x00000000,
    0x00000001,0x00000002,0x0003003e,0x00000009,0x00000015,0x0004003d,
    0x0000000b,0x00000018,0x00000017,0x0004003d,0x0000000f,0x00000019,
    0x00000011,0x00050057,0x00000013,0x0000001a,0x00000018,0x00000019,
    0x0008004f,0x00000007,0x0000001b,0x0000001a,0x0000001a,0x00000000,
    0x00000001,0x00000002,0x0005008e,0x00000007,0x0000001d,0x0000001b,
    0x0000001c,0x00060050,0x00000007,0x0000001f,0x0000001e,0x0000001e,
    0x0000001e,0x00050083,0x00000007,0x00000020,0x0000001d,0x0000001f,
    0x0003003e,0x00000016,0x00000020,0x0004003d,0x0000000b,0x00000024,
    0x00000023,0x0004003d,0x0000000f,0x00000025,0x00000011,0x00050057,
    0x00000013,0x00000026,0x00000024,0x00000025,0x00050051,0x00000006,
    0x00000029,0x00000026,0x00000000,0x0003003e,0x00000022,0x00000029,
    0x0004003d,0x00000007,0x0000002d,0x0000002c,0x0006000c,0x00000007,
    0x0000002e,0x00000001,0x00000045,0x0000002d,0x0003003e,0x0000002a,
    0x0000002e,0x0004003d,0x00000007,0x00000031,0x00000030,0x0006000c,
    0x00000007,0x00000032,0x00000001,0x00000045,0x00000031,0x0003003e,
    0x0000002f,0x00000032,0x0004003d,0x00000007,0x00000034,0x0000002a,
    0x0004003d,0x00000007,0x00000035,0x0000002f,0x0007000c,0x00000007,
    0x00000036,0x00000001,0x00000044,0x00000034,0x00000035,0x0003003e,
    0x00000033,0x00000036,0x0004003d,0x00000007,0x0000003a,0x0000002f,
    0x0004003d,0x00000007,0x0000003b,0x00000033,0x0004003d,0x00000007,
    0x0000003c,0x0000002a,0x00050051,0x00000006,0x0000003e,0x0000003a,
    0x00000000,0x00050051,0x00000006,0x0000003f,0x0000003a,0x00000001,
    0x00050051,0x00000006,0x00000040,0x0000003a,0x00000002,0x00050051,
    0x00000006,0x00000041,0x0000003b,0x00000000,0x00050051,0x00000006,
    0x00000042,0x0000003b,0x00000001,0x00050051,0x00000006,0x00000043,
    0x0000003b,0x00000002,0x00050051,0x00000006,0x00000044,0x0000003c,
    0x00000000,0x00050051,0x00000006,0x00000045,0x0000003c,0x00000001,
    0x00050051,0x00000006,0x00000046,0x0000003c,0x00000002,0x00060050,
    0x00000007,0x00000047,0x0000003e,0x0000003f,0x00000040,0x00060050,
    0x00000007,0x00000048,0x00000041,0x00000042,0x00000043,0x00060050,
    0x00000007,0x00000049,0x00000044,0x00000045,0x00000046,0x00060050,
    0x00000037,0x0000004a,0x00000047,0x00000048,0x00000049,0x0003003e,
    0x00000039,0x0000004a,0x0004003d,0x00000037,0x0000004c,0x00000039,
    0x0004003d,0x00000007,0x0000004d,0x00000016,0x00050091,0x00000007,
    0x0000004e,0x0000004c,0x0000004d,0x0006000c,0x00000007,0x0000004f,
    0x00000001,0x00000045,0x0000004e,0x0003003e,0x0000004b,0x0000004f,
    0x0003003e,0x00000050,0x00000054,0x0004003d,0x00000007,0x00000056,
    0x0000004b,0x0004003d,0x00000007,0x00000057,0x00000050,0x00050094,
    0x00000006,0x00000058,0x00000056,0x00000057,0x0007000c,0x00000006,
    0x00000059,0x00000001,0x00000028,0x00000058,0x0000003d,0x00050085,
    0x00000006,0x0000005b,0x00000059,0x0000005a,0x0004003d,0x00000006,
    0x0000005c,0x00000022,0x00050085,0x00000006,0x0000005d,0x0000005b,
    0x0000005c,0x0003003e,0x00000055,0x0000005d,0x0004003d,0x00000006,
    0x00000061,0x00000022,0x00050085,0x00000006,0x00000062,0x00000060,
    0x00000061,0x00050081,0x00000006,0x00000063,0x0000005f,0x00000062,
    0x0003003e,0x0000005e,0x00000063,0x0004003d,0x00000007,0x00000066,
    0x00000009,0x0004003d,0x00000006,0x00000067,0x00000055,0x0004003d,
    0x00000006,0x00000068,0x0000005e,0x00050081,0x00000006,0x00000069,
    0x00000067,0x00000068,0x0005008e,0x00000007,0x0000006a,0x00000066,
    0x00000069,0x00050051,0x00000006,0x0000006b,0x0000006a,0x00000000,
    0x00050051,0x00000006,0x0000006c,0x0000006a,0x00000001,0x00050051,
    0x00000006,0x0000006d,0x0000006a,0x00000002,0x00070050,0x00000013,
    0x0000006e,0x0000006b,0x0000006c,0x0000006d,0x0000001e,0x0003003e,
    0x00000065,0x0000006e,0x000100fd,0x00010038
};

// Grid vertex shader (GLSL 450):
//   layout(push_constant) uniform PC { mat4 transform; vec4 color; } pc;
//   layout(location=0) in vec3 aPos;
//   void main() { gl_Position = pc.transform * vec4(aPos, 1.0); }
// Note: Original hand-written SPIR-V had ID collision (0x1e used for both
// OpTypePointer and OpCompositeConstruct). Fixed by using ID 0x22 for the
// composite result and bumping the bound from 0x22 to 0x23.
static const uint32_t g_gridVertSpv[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x00000023,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000000, 0x00000004, 0x6e69616d,
    0x00000000, 0x0000000d, 0x00000017, 0x00030003,
    0x00000002, 0x000001c2, 0x00040005, 0x00000004,
    0x6e69616d, 0x00000000, 0x00060005, 0x0000000b,
    0x505f6c67, 0x65567265, 0x78657472, 0x00000000,
    0x00060006, 0x0000000b, 0x00000000, 0x505f6c67,
    0x7469736f, 0x006e6f69, 0x00030005, 0x0000000d,
    0x00000000, 0x00040005, 0x0000000f, 0x00006350,
    0x00000000, 0x00060006, 0x0000000f, 0x00000000,
    0x6e617274, 0x726f6673, 0x0000006d, 0x00050006,
    0x0000000f, 0x00000001, 0x6f6c6f63, 0x00000072,
    0x00030005, 0x00000011, 0x00006370, 0x00040005,
    0x00000017, 0x736f5061, 0x00000000, 0x00050048,
    0x0000000b, 0x00000000, 0x0000000b, 0x00000000,
    0x00030047, 0x0000000b, 0x00000002, 0x00040048,
    0x0000000f, 0x00000000, 0x00000005, 0x00050048,
    0x0000000f, 0x00000000, 0x00000023, 0x00000000,
    0x00050048, 0x0000000f, 0x00000000, 0x00000007,
    0x00000010, 0x00050048, 0x0000000f, 0x00000001,
    0x00000023, 0x00000040, 0x00030047, 0x0000000f,
    0x00000002, 0x00040047, 0x00000017, 0x0000001e,
    0x00000000, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00030016, 0x00000006,
    0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x0003001e, 0x0000000b, 0x00000007,
    0x00040020, 0x0000000c, 0x00000003, 0x0000000b,
    0x0004003b, 0x0000000c, 0x0000000d, 0x00000003,
    0x00040015, 0x0000000e, 0x00000020, 0x00000001,
    0x00040018, 0x00000010, 0x00000007, 0x00000004,
    0x0004001e, 0x0000000f, 0x00000010, 0x00000007,
    0x00040020, 0x00000012, 0x00000009, 0x0000000f,
    0x0004003b, 0x00000012, 0x00000011, 0x00000009,
    0x0004002b, 0x0000000e, 0x00000013, 0x00000000,
    0x00040020, 0x00000014, 0x00000009, 0x00000010,
    0x00040017, 0x00000016, 0x00000006, 0x00000003,
    0x00040020, 0x00000018, 0x00000001, 0x00000016,
    0x0004003b, 0x00000018, 0x00000017, 0x00000001,
    0x0004002b, 0x00000006, 0x0000001a, 0x3f800000,
    0x00040020, 0x0000001e, 0x00000003, 0x00000007,
    0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x00050041,
    0x00000014, 0x00000015, 0x00000011, 0x00000013,
    0x0004003d, 0x00000010, 0x00000019, 0x00000015,
    0x0004003d, 0x00000016, 0x0000001b, 0x00000017,
    0x00050051, 0x00000006, 0x0000001c, 0x0000001b,
    0x00000000, 0x00050051, 0x00000006, 0x0000001d,
    0x0000001b, 0x00000001, 0x00050051, 0x00000006,
    0x0000001f, 0x0000001b, 0x00000002, 0x00070050,
    0x00000007, 0x00000022, 0x0000001c, 0x0000001d,
    0x0000001f, 0x0000001a, 0x00050091, 0x00000007,
    0x00000020, 0x00000019, 0x00000022, 0x00050041,
    0x0000001e, 0x00000021, 0x0000000d, 0x00000013,
    0x0003003e, 0x00000021, 0x00000020, 0x000100fd,
    0x00010038,
};

// Grid fragment shader (GLSL 450):
//   layout(push_constant) uniform PC { mat4 transform; vec4 color; } pc;
//   layout(location=0) out vec4 FragColor;
//   void main() { FragColor = pc.color; }
static const uint32_t g_gridFragSpv[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x00000013,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000004, 0x00000004, 0x6e69616d,
    0x00000000, 0x00000009, 0x00030003, 0x00000002,
    0x000001c2, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00050005, 0x00000009, 0x67617246,
    0x6f6c6f43, 0x00000072, 0x00040005, 0x0000000b,
    0x00006350, 0x00000000, 0x00060006, 0x0000000b,
    0x00000000, 0x6e617274, 0x726f6673, 0x0000006d,
    0x00050006, 0x0000000b, 0x00000001, 0x6f6c6f63,
    0x00000072, 0x00030005, 0x0000000d, 0x00006370,
    0x00040047, 0x00000009, 0x0000001e, 0x00000000,
    0x00040048, 0x0000000b, 0x00000000, 0x00000005,
    0x00050048, 0x0000000b, 0x00000000, 0x00000023,
    0x00000000, 0x00050048, 0x0000000b, 0x00000000,
    0x00000007, 0x00000010, 0x00050048, 0x0000000b,
    0x00000001, 0x00000023, 0x00000040, 0x00030047,
    0x0000000b, 0x00000002, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00030016,
    0x00000006, 0x00000020, 0x00040017, 0x00000007,
    0x00000006, 0x00000004, 0x00040020, 0x00000008,
    0x00000003, 0x00000007, 0x0004003b, 0x00000008,
    0x00000009, 0x00000003, 0x00040018, 0x0000000a,
    0x00000007, 0x00000004, 0x0004001e, 0x0000000b,
    0x0000000a, 0x00000007, 0x00040020, 0x0000000c,
    0x00000009, 0x0000000b, 0x0004003b, 0x0000000c,
    0x0000000d, 0x00000009, 0x00040015, 0x0000000e,
    0x00000020, 0x00000001, 0x0004002b, 0x0000000e,
    0x0000000f, 0x00000001, 0x00040020, 0x00000010,
    0x00000009, 0x00000007, 0x00050036, 0x00000002,
    0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x00050041, 0x00000010, 0x00000011,
    0x0000000d, 0x0000000f, 0x0004003d, 0x00000007,
    0x00000012, 0x00000011,
    // Fix: use actual count from the ID
    0x0003003e, 0x00000009, 0x00000012, 0x000100fd,
    0x00010038,
};

// ============================================================================
// Helper functions
// ============================================================================

static uint32_t FindMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

static bool CreateBuffer(VkDevice device, VkPhysicalDevice physDevice,
    VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
    VkBuffer& buffer, VkDeviceMemory& memory)
{
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, buffer, &memReqs);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(physDevice, memReqs.memoryTypeBits, memProps);

    if (allocInfo.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(device, buffer, memory, 0);
    return true;
}

static VkShaderModule CreateShaderModule(VkDevice device, const uint32_t* code, size_t codeSize) {
    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = codeSize;
    createInfo.pCode = code;

    VkShaderModule module;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

// ============================================================================
// Vertex data
// ============================================================================

struct CubeVertex { float pos[3]; float color[4]; float uv[2]; float normal[3]; float tangent[3]; };
struct GridVertex { float pos[3]; };

// ============================================================================
// Initialization
// ============================================================================

bool InitializeVkRenderer(VkRenderer& renderer, VkDevice device, VkPhysicalDevice physDevice,
    VkQueue queue, uint32_t queueFamilyIndex, VkFormat colorFormat)
{
    renderer.device = device;
    renderer.physicalDevice = physDevice;
    renderer.graphicsQueue = queue;
    renderer.queueFamilyIndex = queueFamilyIndex;

    // Create render pass
    {
        VkAttachmentDescription colorAttach = {};
        colorAttach.format = colorFormat;
        colorAttach.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttach.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription depthAttach = {};
        depthAttach.format = VK_FORMAT_D32_SFLOAT;
        depthAttach.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttach.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription attachments[] = {colorAttach, depthAttach};

        VkAttachmentReference colorRef = {};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthRef = {};
        depthRef.attachment = 1;
        depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;

        VkRenderPassCreateInfo rpInfo = {};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 2;
        rpInfo.pAttachments = attachments;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;

        if (vkCreateRenderPass(device, &rpInfo, nullptr, &renderer.renderPass) != VK_SUCCESS) {
            LOG_ERROR("Failed to create render pass");
            return false;
        }

        // Single render pass with CLEAR for all eyes (no LOAD_OP_LOAD needed)
    }

    // Create pipeline layout with push constants (for grid)
    {
        VkPushConstantRange pushRange = {};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(VkPushConstants);

        VkPipelineLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;

        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &renderer.pipelineLayout) != VK_SUCCESS) {
            LOG_ERROR("Failed to create pipeline layout");
            return false;
        }
    }

    // Descriptor set layout for cube textures (3 combined image samplers)
    {
        VkDescriptorSetLayoutBinding bindings[3] = {};
        for (int i = 0; i < 3; i++) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorSetLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 3;
        layoutInfo.pBindings = bindings;

        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &renderer.descriptorSetLayout) != VK_SUCCESS) {
            LOG_ERROR("Failed to create descriptor set layout");
            return false;
        }
    }

    // Cube pipeline layout: descriptor set + 128-byte push constants
    {
        VkPushConstantRange pushRange = {};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(VkCubePushConstants);  // 128 bytes

        VkPipelineLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &renderer.descriptorSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;

        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &renderer.cubePipelineLayout) != VK_SUCCESS) {
            LOG_ERROR("Failed to create cube pipeline layout");
            return false;
        }
    }

    // Create shader modules
    VkShaderModule cubeVert = CreateShaderModule(device, g_cubeTexturedVertSpv, sizeof(g_cubeTexturedVertSpv));
    VkShaderModule cubeFrag = CreateShaderModule(device, g_cubeTexturedFragSpv, sizeof(g_cubeTexturedFragSpv));
    VkShaderModule gridVert = CreateShaderModule(device, g_gridVertSpv, sizeof(g_gridVertSpv));
    VkShaderModule gridFrag = CreateShaderModule(device, g_gridFragSpv, sizeof(g_gridFragSpv));

    if (!cubeVert || !cubeFrag || !gridVert || !gridFrag) {
        LOG_ERROR("Failed to create shader modules");
        return false;
    }

    // Cube pipeline
    {
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = cubeVert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = cubeFrag;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding = {};
        binding.binding = 0;
        binding.stride = sizeof(CubeVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attrs[5] = {};
        attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = offsetof(CubeVertex, pos);
        attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[1].offset = offsetof(CubeVertex, color);
        attrs[2].location = 2; attrs[2].binding = 0; attrs[2].format = VK_FORMAT_R32G32_SFLOAT; attrs[2].offset = offsetof(CubeVertex, uv);
        attrs[3].location = 3; attrs[3].binding = 0; attrs[3].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[3].offset = offsetof(CubeVertex, normal);
        attrs[4].location = 4; attrs[4].binding = 0; attrs[4].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[4].offset = offsetof(CubeVertex, tangent);

        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 5;
        vertexInput.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState = {};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer = {};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        VkPipelineMultisampleStateCreateInfo multisampling = {};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil = {};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState colorBlendAttach = {};
        colorBlendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlend = {};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &colorBlendAttach;

        VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState = {};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo pipelineInfo = {};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = renderer.cubePipelineLayout;
        pipelineInfo.renderPass = renderer.renderPass;
        pipelineInfo.subpass = 0;

        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &renderer.cubePipeline) != VK_SUCCESS) {
            LOG_ERROR("Failed to create cube pipeline");
            return false;
        }
    }

    // Grid pipeline (line list, no cull)
    {
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = gridVert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = gridFrag;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding = {};
        binding.binding = 0;
        binding.stride = sizeof(GridVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attr = {};
        attr.location = 0;
        attr.binding = 0;
        attr.format = VK_FORMAT_R32G32B32_SFLOAT;
        attr.offset = 0;

        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions = &attr;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

        VkPipelineViewportStateCreateInfo viewportState = {};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer = {};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_NONE;

        VkPipelineMultisampleStateCreateInfo multisampling = {};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil = {};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState colorBlendAttach = {};
        colorBlendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlend = {};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &colorBlendAttach;

        VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState = {};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo pipelineInfo = {};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = renderer.pipelineLayout;
        pipelineInfo.renderPass = renderer.renderPass;
        pipelineInfo.subpass = 0;

        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &renderer.gridPipeline) != VK_SUCCESS) {
            LOG_ERROR("Failed to create grid pipeline");
            return false;
        }
    }

    // Destroy shader modules (no longer needed after pipeline creation)
    vkDestroyShaderModule(device, cubeVert, nullptr);
    vkDestroyShaderModule(device, cubeFrag, nullptr);
    vkDestroyShaderModule(device, gridVert, nullptr);
    vkDestroyShaderModule(device, gridFrag, nullptr);

    // Create vertex/index buffers (host-visible for simplicity)
    CubeVertex cubeVerts[] = {
        // Front face (-Z): normal (0,0,-1), tangent (1,0,0)
        {{-0.5f,-0.5f,-0.5f},{1,1,1,1},{0,1},{0,0,-1},{1,0,0}},
        {{-0.5f, 0.5f,-0.5f},{1,1,1,1},{0,0},{0,0,-1},{1,0,0}},
        {{ 0.5f, 0.5f,-0.5f},{1,1,1,1},{1,0},{0,0,-1},{1,0,0}},
        {{ 0.5f,-0.5f,-0.5f},{1,1,1,1},{1,1},{0,0,-1},{1,0,0}},
        // Back face (+Z): normal (0,0,1), tangent (-1,0,0)
        {{-0.5f,-0.5f, 0.5f},{1,1,1,1},{1,1},{0,0,1},{-1,0,0}},
        {{ 0.5f,-0.5f, 0.5f},{1,1,1,1},{0,1},{0,0,1},{-1,0,0}},
        {{ 0.5f, 0.5f, 0.5f},{1,1,1,1},{0,0},{0,0,1},{-1,0,0}},
        {{-0.5f, 0.5f, 0.5f},{1,1,1,1},{1,0},{0,0,1},{-1,0,0}},
        // Top face (+Y): normal (0,1,0), tangent (1,0,0)
        {{-0.5f, 0.5f,-0.5f},{1,1,1,1},{0,1},{0,1,0},{1,0,0}},
        {{-0.5f, 0.5f, 0.5f},{1,1,1,1},{0,0},{0,1,0},{1,0,0}},
        {{ 0.5f, 0.5f, 0.5f},{1,1,1,1},{1,0},{0,1,0},{1,0,0}},
        {{ 0.5f, 0.5f,-0.5f},{1,1,1,1},{1,1},{0,1,0},{1,0,0}},
        // Bottom face (-Y): normal (0,-1,0), tangent (1,0,0)
        {{-0.5f,-0.5f,-0.5f},{1,1,1,1},{0,0},{0,-1,0},{1,0,0}},
        {{ 0.5f,-0.5f,-0.5f},{1,1,1,1},{1,0},{0,-1,0},{1,0,0}},
        {{ 0.5f,-0.5f, 0.5f},{1,1,1,1},{1,1},{0,-1,0},{1,0,0}},
        {{-0.5f,-0.5f, 0.5f},{1,1,1,1},{0,1},{0,-1,0},{1,0,0}},
        // Left face (-X): normal (-1,0,0), tangent (0,0,-1)
        {{-0.5f,-0.5f, 0.5f},{1,1,1,1},{0,1},{-1,0,0},{0,0,-1}},
        {{-0.5f, 0.5f, 0.5f},{1,1,1,1},{0,0},{-1,0,0},{0,0,-1}},
        {{-0.5f, 0.5f,-0.5f},{1,1,1,1},{1,0},{-1,0,0},{0,0,-1}},
        {{-0.5f,-0.5f,-0.5f},{1,1,1,1},{1,1},{-1,0,0},{0,0,-1}},
        // Right face (+X): normal (1,0,0), tangent (0,0,1)
        {{ 0.5f,-0.5f,-0.5f},{1,1,1,1},{0,1},{1,0,0},{0,0,1}},
        {{ 0.5f, 0.5f,-0.5f},{1,1,1,1},{0,0},{1,0,0},{0,0,1}},
        {{ 0.5f, 0.5f, 0.5f},{1,1,1,1},{1,0},{1,0,0},{0,0,1}},
        {{ 0.5f,-0.5f, 0.5f},{1,1,1,1},{1,1},{1,0,0},{0,0,1}},
    };

    uint16_t cubeIndices[] = {
        0,1,2, 0,2,3,  4,5,6, 4,6,7,  8,9,10, 8,10,11,
        12,13,14, 12,14,15,  16,17,18, 16,18,19,  20,21,22, 20,22,23,
    };

    // Cube vertex buffer
    if (!CreateBuffer(device, physDevice, sizeof(cubeVerts),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.cubeVertexBuffer, renderer.cubeVertexMemory)) {
        LOG_ERROR("Failed to create cube vertex buffer");
        return false;
    }
    void* data;
    vkMapMemory(device, renderer.cubeVertexMemory, 0, sizeof(cubeVerts), 0, &data);
    memcpy(data, cubeVerts, sizeof(cubeVerts));
    vkUnmapMemory(device, renderer.cubeVertexMemory);

    // Cube index buffer
    if (!CreateBuffer(device, physDevice, sizeof(cubeIndices),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.cubeIndexBuffer, renderer.cubeIndexMemory)) {
        LOG_ERROR("Failed to create cube index buffer");
        return false;
    }
    vkMapMemory(device, renderer.cubeIndexMemory, 0, sizeof(cubeIndices), 0, &data);
    memcpy(data, cubeIndices, sizeof(cubeIndices));
    vkUnmapMemory(device, renderer.cubeIndexMemory);

    // Grid vertex buffer
    const int gridSize = 10;
    const float gridSpacing = 1.0f;
    std::vector<GridVertex> gridVerts;
    for (int i = -gridSize; i <= gridSize; i++) {
        gridVerts.push_back({{(float)i * gridSpacing, -1.0f, -gridSize * gridSpacing}});
        gridVerts.push_back({{(float)i * gridSpacing, -1.0f,  gridSize * gridSpacing}});
        gridVerts.push_back({{-gridSize * gridSpacing, -1.0f, (float)i * gridSpacing}});
        gridVerts.push_back({{ gridSize * gridSpacing, -1.0f, (float)i * gridSpacing}});
    }
    renderer.gridVertexCount = (int)gridVerts.size();

    VkDeviceSize gridBufSize = gridVerts.size() * sizeof(GridVertex);
    if (!CreateBuffer(device, physDevice, gridBufSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.gridVertexBuffer, renderer.gridVertexMemory)) {
        LOG_ERROR("Failed to create grid vertex buffer");
        return false;
    }
    vkMapMemory(device, renderer.gridVertexMemory, 0, gridBufSize, 0, &data);
    memcpy(data, gridVerts.data(), (size_t)gridBufSize);
    vkUnmapMemory(device, renderer.gridVertexMemory);

    // Command pool + command buffer
    {
        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndex;

        if (vkCreateCommandPool(device, &poolInfo, nullptr, &renderer.commandPool) != VK_SUCCESS) {
            LOG_ERROR("Failed to create command pool");
            return false;
        }

        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = renderer.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(device, &allocInfo, &renderer.commandBuffer) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate command buffer");
            return false;
        }
    }

    // Load textures
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string exeDir(exePath);
        exeDir = exeDir.substr(0, exeDir.find_last_of("\\/") + 1);
        std::string texDir = exeDir + "textures\\";

        const char* texFiles[3] = {
            "Wood_Crate_001_basecolor.jpg",
            "Wood_Crate_001_normal.jpg",
            "Wood_Crate_001_ambientOcclusion.jpg",
        };

        unsigned char whitePixel[4] = {255, 255, 255, 255};
        unsigned char normalPixel[4] = {128, 128, 255, 255};

        for (int i = 0; i < 3; i++) {
            std::string path = texDir + texFiles[i];
            int w, h, channels;
            unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);

            const unsigned char* srcData;
            if (pixels) {
                srcData = pixels;
                LOG_INFO("Loaded texture: %s (%dx%d)", texFiles[i], w, h);
            } else {
                w = 1; h = 1;
                srcData = (i == 1) ? normalPixel : whitePixel;
                LOG_INFO("Using fallback texture for %s", texFiles[i]);
            }

            // Create VkImage
            VkImageCreateInfo imageInfo = {};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            imageInfo.extent = {(uint32_t)w, (uint32_t)h, 1};
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

            vkCreateImage(device, &imageInfo, nullptr, &renderer.texImages[i]);

            VkMemoryRequirements memReqs;
            vkGetImageMemoryRequirements(device, renderer.texImages[i], &memReqs);

            VkMemoryAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memReqs.size;
            allocInfo.memoryTypeIndex = FindMemoryType(physDevice, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            vkAllocateMemory(device, &allocInfo, nullptr, &renderer.texMemory[i]);
            vkBindImageMemory(device, renderer.texImages[i], renderer.texMemory[i], 0);

            // Upload via staging buffer
            VkDeviceSize imageSize = w * h * 4;
            VkBuffer stagingBuffer;
            VkDeviceMemory stagingMemory;
            CreateBuffer(device, physDevice, imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                stagingBuffer, stagingMemory);

            void* mapped;
            vkMapMemory(device, stagingMemory, 0, imageSize, 0, &mapped);
            memcpy(mapped, srcData, imageSize);
            vkUnmapMemory(device, stagingMemory);

            if (pixels) stbi_image_free(pixels);

            // Record transfer commands
            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkResetCommandBuffer(renderer.commandBuffer, 0);
            vkBeginCommandBuffer(renderer.commandBuffer, &beginInfo);

            // Transition to TRANSFER_DST
            VkImageMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = renderer.texImages[i];
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(renderer.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

            // Copy buffer to image
            VkBufferImageCopy region = {};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
            vkCmdCopyBufferToImage(renderer.commandBuffer, stagingBuffer, renderer.texImages[i],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            // Transition to SHADER_READ
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(renderer.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

            vkEndCommandBuffer(renderer.commandBuffer);

            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &renderer.commandBuffer;
            vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
            vkQueueWaitIdle(queue);

            vkDestroyBuffer(device, stagingBuffer, nullptr);
            vkFreeMemory(device, stagingMemory, nullptr);

            // Create image view
            VkImageViewCreateInfo viewInfo = {};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = renderer.texImages[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            vkCreateImageView(device, &viewInfo, nullptr, &renderer.texViews[i]);
        }

        // Create sampler
        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.maxLod = 1.0f;
        vkCreateSampler(device, &samplerInfo, nullptr, &renderer.texSampler);

        // Create descriptor pool and set
        VkDescriptorPoolSize poolSize = {};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 3;

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &renderer.descriptorPool);

        VkDescriptorSetAllocateInfo setAllocInfo = {};
        setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAllocInfo.descriptorPool = renderer.descriptorPool;
        setAllocInfo.descriptorSetCount = 1;
        setAllocInfo.pSetLayouts = &renderer.descriptorSetLayout;
        vkAllocateDescriptorSets(device, &setAllocInfo, &renderer.descriptorSet);

        // Update descriptor set
        VkDescriptorImageInfo imageInfos[3] = {};
        VkWriteDescriptorSet writes[3] = {};
        for (int i = 0; i < 3; i++) {
            imageInfos[i].sampler = renderer.texSampler;
            imageInfos[i].imageView = renderer.texViews[i];
            imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = renderer.descriptorSet;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &imageInfos[i];
        }
        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
        renderer.texturesLoaded = true;
    }

    // Frame fence
    {
        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        if (vkCreateFence(device, &fenceInfo, nullptr, &renderer.frameFence) != VK_SUCCESS) {
            LOG_ERROR("Failed to create fence");
            return false;
        }
    }

    LOG_INFO("Vulkan renderer initialized");
    return true;
}

bool CreateSwapchainFramebuffers(VkRenderer& renderer, int eye,
    const VkImage* images, uint32_t count,
    uint32_t width, uint32_t height, VkFormat colorFormat)
{
    VkDevice device = renderer.device;

    // Create depth image for this eye
    {
        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_D32_SFLOAT;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

        if (vkCreateImage(device, &imageInfo, nullptr, &renderer.depthImages[eye]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create depth image for eye %d", eye);
            return false;
        }

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, renderer.depthImages[eye], &memReqs);

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(renderer.physicalDevice, memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &renderer.depthMemory[eye]) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate depth memory for eye %d", eye);
            return false;
        }

        vkBindImageMemory(device, renderer.depthImages[eye], renderer.depthMemory[eye], 0);

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = renderer.depthImages[eye];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &renderer.depthViews[eye]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create depth view for eye %d", eye);
            return false;
        }
    }

    // Create image views and framebuffers for each swapchain image
    renderer.swapchainImageViews[eye].resize(count);
    renderer.framebuffers[eye].resize(count);

    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = colorFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &renderer.swapchainImageViews[eye][i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create image view for eye %d image %u", eye, i);
            return false;
        }

        VkImageView attachments[] = {
            renderer.swapchainImageViews[eye][i],
            renderer.depthViews[eye]
        };

        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = renderer.renderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = width;
        fbInfo.height = height;
        fbInfo.layers = 1;

        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &renderer.framebuffers[eye][i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create framebuffer for eye %d image %u", eye, i);
            return false;
        }
    }

    LOG_INFO("Created %u framebuffers for eye %d (%ux%u)", count, eye, width, height);
    return true;
}

void UpdateScene(VkRenderer& renderer, float deltaTime) {
    renderer.cubeRotation += deltaTime * 0.5f;
    if (renderer.cubeRotation > XM_2PI) {
        renderer.cubeRotation -= XM_2PI;
    }
}

void RenderScene(
    VkRenderer& renderer,
    uint32_t imageIndex,
    uint32_t framebufferWidth, uint32_t framebufferHeight,
    const EyeRenderParams* eyes, int eyeCount,
    float zoomScale)
{
    VkDevice device = renderer.device;

    // Wait for previous frame's fence
    LOG_INFO("[RenderScene] imageIndex=%u: vkWaitForFences (pre-render)...", imageIndex);
    VkResult fenceResult = vkWaitForFences(device, 1, &renderer.frameFence, VK_TRUE, UINT64_MAX);
    LOG_INFO("[RenderScene] vkWaitForFences returned %d", (int)fenceResult);
    vkResetFences(device, 1, &renderer.frameFence);

    // Begin command buffer
    vkResetCommandBuffer(renderer.commandBuffer, 0);
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(renderer.commandBuffer, &beginInfo);

    // Single render pass with CLEAR covering full framebuffer
    VkClearValue clearValues[2] = {};
    clearValues[0].color = {{0.05f, 0.05f, 0.25f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin = {};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = renderer.renderPass;
    rpBegin.framebuffer = renderer.framebuffers[0][imageIndex];
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = {framebufferWidth, framebufferHeight};
    rpBegin.clearValueCount = 2;
    rpBegin.pClearValues = clearValues;

    __try {
        vkCmdBeginRenderPass(renderer.commandBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[RenderScene] CRASH in vkCmdBeginRenderPass! exception=0x%08X", GetExceptionCode());
        return;
    }

    // Zoom in eye space: scale only x,y (not z) so perspective division doesn't
    // cancel the effect. Keeps the viewport center fixed on screen.
    XMMATRIX zoom = XMMatrixScaling(zoomScale, zoomScale, 1.0f);

    for (int eye = 0; eye < eyeCount; eye++) {
        uint32_t vpX = eyes[eye].viewportX;
        uint32_t vpY = eyes[eye].viewportY;
        uint32_t w = eyes[eye].width;
        uint32_t h = eyes[eye].height;
        XMMATRIX viewMatrix = eyes[eye].viewMatrix;
        XMMATRIX projMatrix = eyes[eye].projMatrix;

        // Set viewport with Y-flip (negative height) for correct NDC convention
        VkViewport viewport = {};
        viewport.x = (float)vpX;
        viewport.y = (float)(vpY + h);
        viewport.width = (float)w;
        viewport.height = -(float)h;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(renderer.commandBuffer, 0, 1, &viewport);

        VkRect2D scissor = {};
        scissor.offset = {(int32_t)vpX, (int32_t)vpY};
        scissor.extent = {w, h};
        vkCmdSetScissor(renderer.commandBuffer, 0, 1, &scissor);

        // Draw cube - base rests on grid at y=0
        {
            const float cubeSize = 0.06f;
            const float cubeHeight = cubeSize / 2.0f;
            XMMATRIX cubeScale = XMMatrixScaling(cubeSize, cubeSize, cubeSize);
            XMMATRIX cubeRot = XMMatrixRotationY(renderer.cubeRotation);
            XMMATRIX cubeTrans = XMMatrixTranslation(0.0f, cubeHeight, 0.0f);
            XMMATRIX cubeWVP = cubeRot * cubeScale * cubeTrans * viewMatrix * zoom * projMatrix;

            VkCubePushConstants pc = {};
            XMStoreFloat4x4(&pc.mvp, cubeWVP);
            XMMATRIX cubeModel = cubeRot * cubeScale;
            XMStoreFloat4x4(&pc.model, cubeModel);

            vkCmdBindPipeline(renderer.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.cubePipeline);
            vkCmdPushConstants(renderer.commandBuffer, renderer.cubePipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

            if (renderer.texturesLoaded) {
                vkCmdBindDescriptorSets(renderer.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    renderer.cubePipelineLayout, 0, 1, &renderer.descriptorSet, 0, nullptr);
            }

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(renderer.commandBuffer, 0, 1, &renderer.cubeVertexBuffer, &offset);
            vkCmdBindIndexBuffer(renderer.commandBuffer, renderer.cubeIndexBuffer, 0, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(renderer.commandBuffer, 36, 1, 0, 0, 0);
        }

        // Draw grid floor
        {
            const float gridScale = 0.05f;
            XMMATRIX gridWorld = XMMatrixScaling(gridScale, gridScale, gridScale)
                               * XMMatrixTranslation(0, gridScale, 0);
            XMMATRIX gridWVP = gridWorld * viewMatrix * zoom * projMatrix;

            VkPushConstants pc = {};
            XMStoreFloat4x4(&pc.transform, gridWVP);
            pc.color[0] = 0.3f; pc.color[1] = 0.3f; pc.color[2] = 0.35f; pc.color[3] = 1.0f;

            vkCmdBindPipeline(renderer.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.gridPipeline);
            vkCmdPushConstants(renderer.commandBuffer, renderer.pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(renderer.commandBuffer, 0, 1, &renderer.gridVertexBuffer, &offset);
            vkCmdDraw(renderer.commandBuffer, renderer.gridVertexCount, 1, 0, 0);
        }
    }

    vkCmdEndRenderPass(renderer.commandBuffer);
    vkEndCommandBuffer(renderer.commandBuffer);

    // Submit
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &renderer.commandBuffer;

    VkResult submitResult = vkQueueSubmit(renderer.graphicsQueue, 1, &submitInfo, renderer.frameFence);
    LOG_INFO("[RenderScene] vkQueueSubmit returned %d", (int)submitResult);

    // Wait for completion before returning (runtime needs the image ready)
    VkResult waitResult = vkWaitForFences(device, 1, &renderer.frameFence, VK_TRUE, UINT64_MAX);
    LOG_INFO("[RenderScene] vkWaitForFences returned %d - DONE", (int)waitResult);
}

void CleanupVkRenderer(VkRenderer& renderer) {
    if (!renderer.device) return;

    vkDeviceWaitIdle(renderer.device);

    // Cleanup texture resources
    if (renderer.descriptorPool) {
        vkDestroyDescriptorPool(renderer.device, renderer.descriptorPool, nullptr);
        renderer.descriptorPool = VK_NULL_HANDLE;
    }
    if (renderer.texSampler) {
        vkDestroySampler(renderer.device, renderer.texSampler, nullptr);
        renderer.texSampler = VK_NULL_HANDLE;
    }
    for (int i = 0; i < 3; i++) {
        if (renderer.texViews[i]) {
            vkDestroyImageView(renderer.device, renderer.texViews[i], nullptr);
            renderer.texViews[i] = VK_NULL_HANDLE;
        }
        if (renderer.texImages[i]) {
            vkDestroyImage(renderer.device, renderer.texImages[i], nullptr);
            renderer.texImages[i] = VK_NULL_HANDLE;
        }
        if (renderer.texMemory[i]) {
            vkFreeMemory(renderer.device, renderer.texMemory[i], nullptr);
            renderer.texMemory[i] = VK_NULL_HANDLE;
        }
    }
    if (renderer.descriptorSetLayout) {
        vkDestroyDescriptorSetLayout(renderer.device, renderer.descriptorSetLayout, nullptr);
        renderer.descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (renderer.cubePipelineLayout) {
        vkDestroyPipelineLayout(renderer.device, renderer.cubePipelineLayout, nullptr);
        renderer.cubePipelineLayout = VK_NULL_HANDLE;
    }

    for (int eye = 0; eye < 2; eye++) {
        for (auto fb : renderer.framebuffers[eye])
            vkDestroyFramebuffer(renderer.device, fb, nullptr);
        renderer.framebuffers[eye].clear();

        for (auto iv : renderer.swapchainImageViews[eye])
            vkDestroyImageView(renderer.device, iv, nullptr);
        renderer.swapchainImageViews[eye].clear();

        if (renderer.depthViews[eye]) {
            vkDestroyImageView(renderer.device, renderer.depthViews[eye], nullptr);
            renderer.depthViews[eye] = VK_NULL_HANDLE;
        }
        if (renderer.depthImages[eye]) {
            vkDestroyImage(renderer.device, renderer.depthImages[eye], nullptr);
            renderer.depthImages[eye] = VK_NULL_HANDLE;
        }
        if (renderer.depthMemory[eye]) {
            vkFreeMemory(renderer.device, renderer.depthMemory[eye], nullptr);
            renderer.depthMemory[eye] = VK_NULL_HANDLE;
        }
    }

    if (renderer.frameFence) {
        vkDestroyFence(renderer.device, renderer.frameFence, nullptr);
        renderer.frameFence = VK_NULL_HANDLE;
    }
    if (renderer.commandPool) {
        vkDestroyCommandPool(renderer.device, renderer.commandPool, nullptr);
        renderer.commandPool = VK_NULL_HANDLE;
    }

    if (renderer.gridVertexBuffer) {
        vkDestroyBuffer(renderer.device, renderer.gridVertexBuffer, nullptr);
        renderer.gridVertexBuffer = VK_NULL_HANDLE;
    }
    if (renderer.gridVertexMemory) {
        vkFreeMemory(renderer.device, renderer.gridVertexMemory, nullptr);
        renderer.gridVertexMemory = VK_NULL_HANDLE;
    }
    if (renderer.cubeIndexBuffer) {
        vkDestroyBuffer(renderer.device, renderer.cubeIndexBuffer, nullptr);
        renderer.cubeIndexBuffer = VK_NULL_HANDLE;
    }
    if (renderer.cubeIndexMemory) {
        vkFreeMemory(renderer.device, renderer.cubeIndexMemory, nullptr);
        renderer.cubeIndexMemory = VK_NULL_HANDLE;
    }
    if (renderer.cubeVertexBuffer) {
        vkDestroyBuffer(renderer.device, renderer.cubeVertexBuffer, nullptr);
        renderer.cubeVertexBuffer = VK_NULL_HANDLE;
    }
    if (renderer.cubeVertexMemory) {
        vkFreeMemory(renderer.device, renderer.cubeVertexMemory, nullptr);
        renderer.cubeVertexMemory = VK_NULL_HANDLE;
    }

    if (renderer.gridPipeline) {
        vkDestroyPipeline(renderer.device, renderer.gridPipeline, nullptr);
        renderer.gridPipeline = VK_NULL_HANDLE;
    }
    if (renderer.cubePipeline) {
        vkDestroyPipeline(renderer.device, renderer.cubePipeline, nullptr);
        renderer.cubePipeline = VK_NULL_HANDLE;
    }
    if (renderer.pipelineLayout) {
        vkDestroyPipelineLayout(renderer.device, renderer.pipelineLayout, nullptr);
        renderer.pipelineLayout = VK_NULL_HANDLE;
    }
    if (renderer.renderPass) {
        vkDestroyRenderPass(renderer.device, renderer.renderPass, nullptr);
        renderer.renderPass = VK_NULL_HANDLE;
    }
}
