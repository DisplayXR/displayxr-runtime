// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Cross-platform Vulkan OpenXR spinning cube test app (legacy variant)
 *
 * Self-contained single-file app that renders a spinning cube + grid floor
 * via Vulkan + OpenXR. Designed for the sim_display driver on macOS but
 * works on any platform with a Vulkan-capable OpenXR runtime.
 *
 * Legacy variant: does NOT enable XR_EXT_display_info. Relies on runtime
 * defaults and recommendedImageRectWidth for swapchain sizing.
 *
 * No windowing code — the runtime's compositor creates its own window.
 * No input handling — static camera with continuous cube rotation.
 */

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
// Legacy app: NO DisplayXR extension types needed.
// This app uses only standard OpenXR APIs.

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// ============================================================================
// Logging
// ============================================================================

#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#define XR_CHECK(call) \
    do { \
        XrResult _r = (call); \
        if (XR_FAILED(_r)) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

#define VK_CHECK(call) \
    do { \
        VkResult _r = (call); \
        if (_r != VK_SUCCESS) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

// ============================================================================
// Inline math — column-major float[16] matrices
// ============================================================================
// Column-major layout: m[col*4 + row]
// SPIR-V shaders declare push constants with ColMajor decoration,
// so column-major data is read directly as the matrix M.
// Shader computes gl_Position = M * vec4(pos, 1.0).

static void mat4_identity(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_multiply(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            tmp[col * 4 + row] = sum;
        }
    }
    memcpy(out, tmp, sizeof(tmp));
}

static void mat4_translation(float* m, float tx, float ty, float tz) {
    mat4_identity(m);
    m[12] = tx;
    m[13] = ty;
    m[14] = tz;
}

static void mat4_scaling(float* m, float sx, float sy, float sz) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = sx;
    m[5] = sy;
    m[10] = sz;
    m[15] = 1.0f;
}

static void mat4_rotation_y(float* m, float angle) {
    float c = cosf(angle), s = sinf(angle);
    mat4_identity(m);
    m[0] = c;
    m[8] = s;
    m[2] = -s;
    m[10] = c;
}

// Asymmetric projection from XrFovf (OpenGL-style infinite-far optional)
static void mat4_from_xr_fov(float* m, const XrFovf& fov, float nearZ, float farZ) {
    float left = nearZ * tanf(fov.angleLeft);
    float right = nearZ * tanf(fov.angleRight);
    float top = nearZ * tanf(fov.angleUp);
    float bottom = nearZ * tanf(fov.angleDown);

    float w = right - left;
    float h = top - bottom;

    memset(m, 0, 16 * sizeof(float));
    // Column-major: m[col*4 + row]
    m[0]  = 2.0f * nearZ / w;                        // col 0, row 0
    m[5]  = 2.0f * nearZ / h;                        // col 1, row 1
    m[8]  = (right + left) / w;                       // col 2, row 0
    m[9]  = (top + bottom) / h;                       // col 2, row 1
    m[10] = -(farZ + nearZ) / (farZ - nearZ);         // col 2, row 2
    m[11] = -1.0f;                                     // col 2, row 3
    m[14] = -2.0f * farZ * nearZ / (farZ - nearZ);    // col 3, row 2
}

// View matrix from XR pose: V = R^T * T(-pos)
static void mat4_view_from_xr_pose(float* m, const XrPosef& pose) {
    float qx = pose.orientation.x;
    float qy = pose.orientation.y;
    float qz = pose.orientation.z;
    float qw = pose.orientation.w;

    // Rotation matrix from quaternion (R)
    float r00 = 1.0f - 2.0f * (qy * qy + qz * qz);
    float r01 = 2.0f * (qx * qy - qz * qw);
    float r02 = 2.0f * (qx * qz + qy * qw);

    float r10 = 2.0f * (qx * qy + qz * qw);
    float r11 = 1.0f - 2.0f * (qx * qx + qz * qz);
    float r12 = 2.0f * (qy * qz - qx * qw);

    float r20 = 2.0f * (qx * qz - qy * qw);
    float r21 = 2.0f * (qy * qz + qx * qw);
    float r22 = 1.0f - 2.0f * (qx * qx + qy * qy);

    // R^T (inverse of rotation) — swap indices
    // R^T[i][j] = R[j][i]
    float px = pose.position.x;
    float py = pose.position.y;
    float pz = pose.position.z;

    // Translation: -R^T * pos
    float tx = -(r00 * px + r10 * py + r20 * pz);
    float ty = -(r01 * px + r11 * py + r21 * pz);
    float tz = -(r02 * px + r12 * py + r22 * pz);

    // Column-major: m[col*4 + row]
    // Row 0: R^T[0][0], R^T[0][1], R^T[0][2], tx
    // Row 1: R^T[1][0], R^T[1][1], R^T[1][2], ty
    // Row 2: R^T[2][0], R^T[2][1], R^T[2][2], tz
    // Row 3: 0,          0,          0,          1
    m[0]  = r00;  m[4]  = r10;  m[8]  = r20;  m[12] = tx;
    m[1]  = r01;  m[5]  = r11;  m[9]  = r21;  m[13] = ty;
    m[2]  = r02;  m[6]  = r12;  m[10] = r22;  m[14] = tz;
    m[3]  = 0.0f; m[7]  = 0.0f; m[11] = 0.0f; m[15] = 1.0f;
}

// ============================================================================
// Texture path helper — cross-platform executable-relative path
// ============================================================================

static std::string GetTextureDir() {
    char pathBuf[4096];
#ifdef __APPLE__
    uint32_t pathSize = sizeof(pathBuf);
    if (_NSGetExecutablePath(pathBuf, &pathSize) == 0) {
        char* lastSlash = strrchr(pathBuf, '/');
        if (lastSlash) *lastSlash = '\0';
        return std::string(pathBuf) + "/textures/";
    }
#elif defined(_WIN32)
    DWORD len = GetModuleFileNameA(NULL, pathBuf, sizeof(pathBuf));
    if (len > 0 && len < sizeof(pathBuf)) {
        char* lastSlash = strrchr(pathBuf, '\\');
        if (lastSlash) *lastSlash = '\0';
        return std::string(pathBuf) + "\\textures\\";
    }
#else
    ssize_t len = readlink("/proc/self/exe", pathBuf, sizeof(pathBuf) - 1);
    if (len > 0) {
        pathBuf[len] = '\0';
        char* lastSlash = strrchr(pathBuf, '/');
        if (lastSlash) *lastSlash = '\0';
        return std::string(pathBuf) + "/textures/";
    }
#endif
    return "textures/";
}

// ============================================================================
// Embedded SPIR-V shaders (copied from vk_renderer.cpp)
// ============================================================================

// Textured cube vertex shader: push constants (MVP + model), outputs UV/normal/tangent
//   layout(push_constant) uniform PushConstants { mat4 mvp; mat4 model; };
//   in: position(0), color(1), uv(2), normal(3), tangent(4)
//   out: fragUV(0), fragWorldNormal(1), fragWorldTangent(2)
static const uint32_t g_cubeVertSpv[] = {
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

// Textured cube fragment shader: samples basecolor, normal, AO textures with directional lighting
//   binding 0: basecolor sampler, binding 1: normal sampler, binding 2: AO sampler
//   in: fragUV(0), fragWorldNormal(1), fragWorldTangent(2)
//   Constructs TBN matrix, applies normal mapping, directional light with AO
static const uint32_t g_cubeFragSpv[] = {
    0x07230203,0x00010000,0x0008000b,0x00000071,0x00000000,0x00020011,
    0x00000001,0x0006000b,0x00000001,0x4c534c47,0x6474732e,0x3035342e,
    0x00000000,0x0003000e,0x00000000,0x00000001,0x0009000f,0x00000004,
    0x00000004,0x6e69616d,0x00000000,0x00000011,0x00000027,0x0000002b,
    0x00000067,0x00030010,0x00000004,0x00000007,0x00030003,0x00000002,
    0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00050005,
    0x00000009,0x65736162,0x6f6c6f63,0x00000072,0x00060005,0x0000000d,
    0x65736162,0x6f6c6f63,0x78655472,0x00000000,0x00040005,0x00000011,
    0x67617266,0x00005655,0x00050005,0x00000016,0x6d726f6e,0x614d6c61,
    0x00000070,0x00050005,0x00000017,0x6d726f6e,0x65546c61,0x00000078,
    0x00030005,0x0000001d,0x00006f61,0x00040005,0x0000001e,0x65546f61,
    0x00000078,0x00030005,0x00000025,0x0000004e,0x00060005,0x00000027,
    0x67617266,0x6c726f57,0x726f4e64,0x006c616d,0x00030005,0x0000002a,
    0x00000054,0x00070005,0x0000002b,0x67617266,0x6c726f57,0x6e615464,
    0x746e6567,0x00000000,0x00030005,0x00000036,0x00000042,0x00030005,
    0x0000003c,0x004e4254,0x00060005,0x0000004f,0x7070616d,0x6f4e6465,
    0x6c616d72,0x00000000,0x00050005,0x00000058,0x6867696c,0x72694474,
    0x00000000,0x00040005,0x0000005d,0x66666964,0x00657375,0x00050005,
    0x00000067,0x4374756f,0x726f6c6f,0x00000000,0x00040047,0x0000000d,
    0x00000021,0x00000000,0x00040047,0x0000000d,0x00000022,0x00000000,
    0x00040047,0x00000011,0x0000001e,0x00000000,0x00040047,0x00000017,
    0x00000021,0x00000001,0x00040047,0x00000017,0x00000022,0x00000000,
    0x00040047,0x0000001e,0x00000021,0x00000002,0x00040047,0x0000001e,
    0x00000022,0x00000000,0x00040047,0x00000027,0x0000001e,0x00000001,
    0x00040047,0x0000002b,0x0000001e,0x00000002,0x00040047,0x00000067,
    0x0000001e,0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,
    0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,
    0x00000006,0x00000003,0x00040020,0x00000008,0x00000007,0x00000007,
    0x00090019,0x0000000a,0x00000006,0x00000001,0x00000000,0x00000000,
    0x00000000,0x00000001,0x00000000,0x0003001b,0x0000000b,0x0000000a,
    0x00040020,0x0000000c,0x00000000,0x0000000b,0x0004003b,0x0000000c,
    0x0000000d,0x00000000,0x00040017,0x0000000f,0x00000006,0x00000002,
    0x00040020,0x00000010,0x00000001,0x0000000f,0x0004003b,0x00000010,
    0x00000011,0x00000001,0x00040017,0x00000013,0x00000006,0x00000004,
    0x0004003b,0x0000000c,0x00000017,0x00000000,0x00040020,0x0000001c,
    0x00000007,0x00000006,0x0004003b,0x0000000c,0x0000001e,0x00000000,
    0x00040015,0x00000022,0x00000020,0x00000000,0x0004002b,0x00000022,
    0x00000023,0x00000000,0x00040020,0x00000026,0x00000001,0x00000007,
    0x0004003b,0x00000026,0x00000027,0x00000001,0x0004003b,0x00000026,
    0x0000002b,0x00000001,0x00040018,0x0000003a,0x00000007,0x00000003,
    0x00040020,0x0000003b,0x00000007,0x0000003a,0x0004002b,0x00000006,
    0x00000040,0x3f800000,0x0004002b,0x00000006,0x00000041,0x00000000,
    0x0004002b,0x00000006,0x00000052,0x40000000,0x0004002b,0x00000006,
    0x00000059,0x3e9b28d0,0x0004002b,0x00000006,0x0000005a,0x3f4ee116,
    0x0004002b,0x00000006,0x0000005b,0x3f014cae,0x0006002c,0x00000007,
    0x0000005c,0x00000059,0x0000005a,0x0000005b,0x0004002b,0x00000006,
    0x00000062,0x3f333333,0x0004002b,0x00000006,0x00000064,0x3e99999a,
    0x00040020,0x00000066,0x00000003,0x00000013,0x0004003b,0x00000066,
    0x00000067,0x00000003,0x00050036,0x00000002,0x00000004,0x00000000,
    0x00000003,0x000200f8,0x00000005,0x0004003b,0x00000008,0x00000009,
    0x00000007,0x0004003b,0x00000008,0x00000016,0x00000007,0x0004003b,
    0x0000001c,0x0000001d,0x00000007,0x0004003b,0x00000008,0x00000025,
    0x00000007,0x0004003b,0x00000008,0x0000002a,0x00000007,0x0004003b,
    0x00000008,0x00000036,0x00000007,0x0004003b,0x0000003b,0x0000003c,
    0x00000007,0x0004003b,0x00000008,0x0000004f,0x00000007,0x0004003b,
    0x00000008,0x00000058,0x00000007,0x0004003b,0x0000001c,0x0000005d,
    0x00000007,0x0004003d,0x0000000b,0x0000000e,0x0000000d,0x0004003d,
    0x0000000f,0x00000012,0x00000011,0x00050057,0x00000013,0x00000014,
    0x0000000e,0x00000012,0x0008004f,0x00000007,0x00000015,0x00000014,
    0x00000014,0x00000000,0x00000001,0x00000002,0x0003003e,0x00000009,
    0x00000015,0x0004003d,0x0000000b,0x00000018,0x00000017,0x0004003d,
    0x0000000f,0x00000019,0x00000011,0x00050057,0x00000013,0x0000001a,
    0x00000018,0x00000019,0x0008004f,0x00000007,0x0000001b,0x0000001a,
    0x0000001a,0x00000000,0x00000001,0x00000002,0x0003003e,0x00000016,
    0x0000001b,0x0004003d,0x0000000b,0x0000001f,0x0000001e,0x0004003d,
    0x0000000f,0x00000020,0x00000011,0x00050057,0x00000013,0x00000021,
    0x0000001f,0x00000020,0x00050051,0x00000006,0x00000024,0x00000021,
    0x00000000,0x0003003e,0x0000001d,0x00000024,0x0004003d,0x00000007,
    0x00000028,0x00000027,0x0006000c,0x00000007,0x00000029,0x00000001,
    0x00000045,0x00000028,0x0003003e,0x00000025,0x00000029,0x0004003d,
    0x00000007,0x0000002c,0x0000002b,0x0006000c,0x00000007,0x0000002d,
    0x00000001,0x00000045,0x0000002c,0x0003003e,0x0000002a,0x0000002d,
    0x0004003d,0x00000007,0x0000002e,0x0000002a,0x0004003d,0x00000007,
    0x0000002f,0x0000002a,0x0004003d,0x00000007,0x00000030,0x00000025,
    0x00050094,0x00000006,0x00000031,0x0000002f,0x00000030,0x0004003d,
    0x00000007,0x00000032,0x00000025,0x0005008e,0x00000007,0x00000033,
    0x00000032,0x00000031,0x00050083,0x00000007,0x00000034,0x0000002e,
    0x00000033,0x0006000c,0x00000007,0x00000035,0x00000001,0x00000045,
    0x00000034,0x0003003e,0x0000002a,0x00000035,0x0004003d,0x00000007,
    0x00000037,0x00000025,0x0004003d,0x00000007,0x00000038,0x0000002a,
    0x0007000c,0x00000007,0x00000039,0x00000001,0x00000044,0x00000037,
    0x00000038,0x0003003e,0x00000036,0x00000039,0x0004003d,0x00000007,
    0x0000003d,0x0000002a,0x0004003d,0x00000007,0x0000003e,0x00000036,
    0x0004003d,0x00000007,0x0000003f,0x00000025,0x00050051,0x00000006,
    0x00000042,0x0000003d,0x00000000,0x00050051,0x00000006,0x00000043,
    0x0000003d,0x00000001,0x00050051,0x00000006,0x00000044,0x0000003d,
    0x00000002,0x00050051,0x00000006,0x00000045,0x0000003e,0x00000000,
    0x00050051,0x00000006,0x00000046,0x0000003e,0x00000001,0x00050051,
    0x00000006,0x00000047,0x0000003e,0x00000002,0x00050051,0x00000006,
    0x00000048,0x0000003f,0x00000000,0x00050051,0x00000006,0x00000049,
    0x0000003f,0x00000001,0x00050051,0x00000006,0x0000004a,0x0000003f,
    0x00000002,0x00060050,0x00000007,0x0000004b,0x00000042,0x00000043,
    0x00000044,0x00060050,0x00000007,0x0000004c,0x00000045,0x00000046,
    0x00000047,0x00060050,0x00000007,0x0000004d,0x00000048,0x00000049,
    0x0000004a,0x00060050,0x0000003a,0x0000004e,0x0000004b,0x0000004c,
    0x0000004d,0x0003003e,0x0000003c,0x0000004e,0x0004003d,0x0000003a,
    0x00000050,0x0000003c,0x0004003d,0x00000007,0x00000051,0x00000016,
    0x0005008e,0x00000007,0x00000053,0x00000051,0x00000052,0x00060050,
    0x00000007,0x00000054,0x00000040,0x00000040,0x00000040,0x00050083,
    0x00000007,0x00000055,0x00000053,0x00000054,0x00050091,0x00000007,
    0x00000056,0x00000050,0x00000055,0x0006000c,0x00000007,0x00000057,
    0x00000001,0x00000045,0x00000056,0x0003003e,0x0000004f,0x00000057,
    0x0003003e,0x00000058,0x0000005c,0x0004003d,0x00000007,0x0000005e,
    0x0000004f,0x0004003d,0x00000007,0x0000005f,0x00000058,0x00050094,
    0x00000006,0x00000060,0x0000005e,0x0000005f,0x0007000c,0x00000006,
    0x00000061,0x00000001,0x00000028,0x00000060,0x00000041,0x00050085,
    0x00000006,0x00000063,0x00000061,0x00000062,0x00050081,0x00000006,
    0x00000065,0x00000063,0x00000064,0x0003003e,0x0000005d,0x00000065,
    0x0004003d,0x00000007,0x00000068,0x00000009,0x0004003d,0x00000006,
    0x00000069,0x0000001d,0x0005008e,0x00000007,0x0000006a,0x00000068,
    0x00000069,0x0004003d,0x00000006,0x0000006b,0x0000005d,0x0005008e,
    0x00000007,0x0000006c,0x0000006a,0x0000006b,0x00050051,0x00000006,
    0x0000006d,0x0000006c,0x00000000,0x00050051,0x00000006,0x0000006e,
    0x0000006c,0x00000001,0x00050051,0x00000006,0x0000006f,0x0000006c,
    0x00000002,0x00070050,0x00000013,0x00000070,0x0000006d,0x0000006e,
    0x0000006f,0x00000040,0x0003003e,0x00000067,0x00000070,0x000100fd,
    0x00010038,
};

// Grid vertex shader:
//   layout(push_constant) uniform PC { mat4 transform; vec4 color; } pc;
//   layout(location=0) in vec3 aPos;
//   void main() { gl_Position = pc.transform * vec4(aPos, 1.0); }
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

// Grid fragment shader:
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
    0x0003003e, 0x00000009, 0x00000012, 0x000100fd,
    0x00010038,
};

// ============================================================================
// Vulkan helpers (from vk_renderer.cpp)
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

// Load a texture from disk, create VkImage + VkImageView, upload via staging buffer.
// If the file can't be loaded, creates a 1x1 fallback (white for basecolor/AO, flat blue for normal).
static bool CreateTextureFromFile(VkDevice device, VkPhysicalDevice physDevice,
    VkCommandPool cmdPool, VkQueue queue,
    const char* path, bool isNormalMap,
    VkImage& outImage, VkDeviceMemory& outMemory, VkImageView& outView)
{
    int w, h, channels;
    unsigned char* pixels = stbi_load(path, &w, &h, &channels, 4); // force RGBA
    bool fallback = false;
    if (!pixels) {
        LOG_WARN("Failed to load texture: %s — using fallback", path);
        w = h = 1;
        static unsigned char whitePixel[] = {255, 255, 255, 255};
        static unsigned char bluePixel[] = {128, 128, 255, 255}; // flat normal
        pixels = isNormalMap ? bluePixel : whitePixel;
        fallback = true;
    }

    VkDeviceSize imageSize = (VkDeviceSize)w * h * 4;

    // Staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    VkBufferCreateInfo bufInfo = {};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = imageSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &bufInfo, nullptr, &stagingBuffer);

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, stagingBuffer, &memReqs);
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(physDevice, memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
    vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

    void* data;
    vkMapMemory(device, stagingMemory, 0, imageSize, 0, &data);
    memcpy(data, pixels, (size_t)imageSize);
    vkUnmapMemory(device, stagingMemory);
    if (!fallback) stbi_image_free(pixels);

    // Create image
    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent = {(uint32_t)w, (uint32_t)h, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(device, &imgInfo, nullptr, &outImage);

    vkGetImageMemoryRequirements(device, outImage, &memReqs);
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(physDevice, memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device, &allocInfo, nullptr, &outMemory);
    vkBindImageMemory(device, outImage, outMemory, 0);

    // Copy staging -> image via one-shot command buffer
    VkCommandBufferAllocateInfo cmdAlloc = {};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = cmdPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &cmdAlloc, &cmd);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Transition UNDEFINED -> TRANSFER_DST
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = outImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuffer, outImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition TRANSFER_DST -> SHADER_READ_ONLY
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);

    // Cleanup staging
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);

    // Create image view
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device, &viewInfo, nullptr, &outView);

    return !fallback;
}

// ============================================================================
// Vertex types + renderer struct
// ============================================================================

struct CubeVertex { float pos[3]; float color[4]; float uv[2]; float normal[3]; float tangent[3]; };
struct GridVertex { float pos[3]; };

struct PushConstants {
    float transform[16];
    float color[4];
};

struct EyeRenderParams {
    uint32_t viewportX, viewportY, width, height;
    float viewMat[16];
    float projMat[16];
};

struct VkRenderer {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;        // grid: push constants only (80 bytes)
    VkPipelineLayout cubePipelineLayout = VK_NULL_HANDLE;    // textured cube: descriptor set + push constants (128 bytes)
    VkPipeline cubePipeline = VK_NULL_HANDLE;
    VkPipeline gridPipeline = VK_NULL_HANDLE;

    VkBuffer cubeVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cubeVertexMemory = VK_NULL_HANDLE;
    VkBuffer cubeIndexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cubeIndexMemory = VK_NULL_HANDLE;

    VkBuffer gridVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory gridVertexMemory = VK_NULL_HANDLE;
    int gridVertexCount = 0;

    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;

    std::vector<VkImageView> swapchainImageViews;
    std::vector<VkFramebuffer> framebuffers;

    uint32_t fbWidth = 0;
    uint32_t fbHeight = 0;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence frameFence = VK_NULL_HANDLE;

    float cubeRotation = 0.0f;

    // Texture resources
    VkImage texImages[3] = {};          // basecolor, normal, AO
    VkDeviceMemory texMemory[3] = {};
    VkImageView texViews[3] = {};
    VkSampler texSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    bool texturesLoaded = false;
};

// ============================================================================
// Renderer init / framebuffers / cleanup (ported from vk_renderer.cpp)
// ============================================================================

static bool InitializeVkRenderer(VkRenderer& renderer, VkDevice device, VkPhysicalDevice physDevice,
    VkQueue queue, uint32_t queueFamilyIndex, VkFormat colorFormat)
{
    renderer.device = device;
    renderer.physicalDevice = physDevice;
    renderer.graphicsQueue = queue;
    renderer.queueFamilyIndex = queueFamilyIndex;

    // Render pass
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
    }

    // Grid pipeline layout (push constants only, 80 bytes: mat4 + vec4)
    {
        VkPushConstantRange pushRange = {};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(PushConstants);

        VkPipelineLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;

        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &renderer.pipelineLayout) != VK_SUCCESS) {
            LOG_ERROR("Failed to create grid pipeline layout");
            return false;
        }
    }

    // Descriptor set layout for textured cube (3 combined image samplers)
    {
        VkDescriptorSetLayoutBinding bindings[3] = {};
        for (int i = 0; i < 3; i++) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dslInfo = {};
        dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 3;
        dslInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &renderer.descriptorSetLayout) != VK_SUCCESS) {
            LOG_ERROR("Failed to create descriptor set layout");
            return false;
        }
    }

    // Cube pipeline layout (descriptor set + 128 bytes push constants: MVP + model)
    {
        VkPushConstantRange cubePushRange = {};
        cubePushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        cubePushRange.offset = 0;
        cubePushRange.size = 128; // MVP + model matrices

        VkPipelineLayoutCreateInfo cubeLayoutInfo = {};
        cubeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        cubeLayoutInfo.setLayoutCount = 1;
        cubeLayoutInfo.pSetLayouts = &renderer.descriptorSetLayout;
        cubeLayoutInfo.pushConstantRangeCount = 1;
        cubeLayoutInfo.pPushConstantRanges = &cubePushRange;

        if (vkCreatePipelineLayout(device, &cubeLayoutInfo, nullptr, &renderer.cubePipelineLayout) != VK_SUCCESS) {
            LOG_ERROR("Failed to create cube pipeline layout");
            return false;
        }
    }

    // Shader modules
    VkShaderModule cubeVert = CreateShaderModule(device, g_cubeVertSpv, sizeof(g_cubeVertSpv));
    VkShaderModule cubeFrag = CreateShaderModule(device, g_cubeFragSpv, sizeof(g_cubeFragSpv));
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
        attrs[0].location = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = offsetof(CubeVertex, pos);
        attrs[1].location = 1; attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[1].offset = offsetof(CubeVertex, color);
        attrs[2].location = 2; attrs[2].format = VK_FORMAT_R32G32_SFLOAT; attrs[2].offset = offsetof(CubeVertex, uv);
        attrs[3].location = 3; attrs[3].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[3].offset = offsetof(CubeVertex, normal);
        attrs[4].location = 4; attrs[4].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[4].offset = offsetof(CubeVertex, tangent);

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
        rasterizer.cullMode = VK_CULL_MODE_NONE;
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

    vkDestroyShaderModule(device, cubeVert, nullptr);
    vkDestroyShaderModule(device, cubeFrag, nullptr);
    vkDestroyShaderModule(device, gridVert, nullptr);
    vkDestroyShaderModule(device, gridFrag, nullptr);

    // Vertex/index buffers — textured cube with UV, normal, tangent per vertex
    //   pos[3], color[4], uv[2], normal[3], tangent[3] = 15 floats per vertex
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

    // Load textures and create descriptor set
    {
        std::string texDir = GetTextureDir();
        const char* texFiles[3] = {
            "Wood_Crate_001_basecolor.jpg",
            "Wood_Crate_001_normal.jpg",
            "Wood_Crate_001_ambientOcclusion.jpg",
        };
        bool isNormal[3] = {false, true, false};
        renderer.texturesLoaded = true;
        for (int i = 0; i < 3; i++) {
            std::string path = texDir + texFiles[i];
            if (!CreateTextureFromFile(device, physDevice, renderer.commandPool,
                    renderer.graphicsQueue, path.c_str(), isNormal[i],
                    renderer.texImages[i], renderer.texMemory[i], renderer.texViews[i])) {
                renderer.texturesLoaded = false;
            }
        }

        // Sampler (linear filtering, repeat wrap)
        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.maxLod = 1.0f;
        if (vkCreateSampler(device, &samplerInfo, nullptr, &renderer.texSampler) != VK_SUCCESS) {
            LOG_ERROR("Failed to create texture sampler");
            return false;
        }

        // Descriptor pool and set
        VkDescriptorPoolSize poolSize = {};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 3;
        VkDescriptorPoolCreateInfo dpInfo = {};
        dpInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpInfo.maxSets = 1;
        dpInfo.poolSizeCount = 1;
        dpInfo.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(device, &dpInfo, nullptr, &renderer.descriptorPool) != VK_SUCCESS) {
            LOG_ERROR("Failed to create descriptor pool");
            return false;
        }

        VkDescriptorSetAllocateInfo dsAllocInfo = {};
        dsAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsAllocInfo.descriptorPool = renderer.descriptorPool;
        dsAllocInfo.descriptorSetCount = 1;
        dsAllocInfo.pSetLayouts = &renderer.descriptorSetLayout;
        if (vkAllocateDescriptorSets(device, &dsAllocInfo, &renderer.descriptorSet) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate descriptor set");
            return false;
        }

        // Update descriptor set with texture views
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

        if (renderer.texturesLoaded) {
            LOG_INFO("All crate textures loaded successfully");
        } else {
            LOG_WARN("Some textures missing — using fallback colors");
        }
    }

    LOG_INFO("Vulkan renderer initialized");
    return true;
}

static bool CreateSwapchainFramebuffers(VkRenderer& renderer,
    const VkImage* images, uint32_t count,
    uint32_t width, uint32_t height, VkFormat colorFormat)
{
    VkDevice device = renderer.device;
    renderer.fbWidth = width;
    renderer.fbHeight = height;

    // Single depth image for the full SBS framebuffer
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

        if (vkCreateImage(device, &imageInfo, nullptr, &renderer.depthImage) != VK_SUCCESS) {
            LOG_ERROR("Failed to create depth image");
            return false;
        }

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, renderer.depthImage, &memReqs);

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(renderer.physicalDevice, memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &renderer.depthMemory) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate depth memory");
            return false;
        }

        vkBindImageMemory(device, renderer.depthImage, renderer.depthMemory, 0);

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = renderer.depthImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &renderer.depthView) != VK_SUCCESS) {
            LOG_ERROR("Failed to create depth view");
            return false;
        }
    }

    // Image views and framebuffers for each swapchain image
    renderer.swapchainImageViews.resize(count);
    renderer.framebuffers.resize(count);

    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = colorFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &renderer.swapchainImageViews[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create image view for image %u", i);
            return false;
        }

        VkImageView attachments[] = {
            renderer.swapchainImageViews[i],
            renderer.depthView
        };

        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = renderer.renderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = width;
        fbInfo.height = height;
        fbInfo.layers = 1;

        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &renderer.framebuffers[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create framebuffer for image %u", i);
            return false;
        }
    }

    LOG_INFO("Created %u framebuffers (%ux%u)", count, width, height);
    return true;
}

static void RenderScene(
    VkRenderer& renderer, uint32_t imageIndex,
    const EyeRenderParams* eyes, int eyeCount)
{
    VkDevice device = renderer.device;

    // Wait for previous frame
    vkWaitForFences(device, 1, &renderer.frameFence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &renderer.frameFence);

    // Begin command buffer
    VkCommandBuffer cmd = renderer.commandBuffer;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Begin render pass with full SBS framebuffer
    VkClearValue clearValues[2] = {};
    clearValues[0].color = {{0.05f, 0.05f, 0.25f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin = {};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = renderer.renderPass;
    rpBegin.framebuffer = renderer.framebuffers[imageIndex];
    rpBegin.renderArea = {{0, 0}, {renderer.fbWidth, renderer.fbHeight}};
    rpBegin.clearValueCount = 2;
    rpBegin.pClearValues = clearValues;

    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // Render all eyes in a single render pass — just change viewport/scissor
    for (int eye = 0; eye < eyeCount; eye++) {
        const auto& e = eyes[eye];

        // Viewport with Y-flip for correct NDC convention
        VkViewport viewport = {(float)e.viewportX, (float)(e.viewportY + e.height),
            (float)e.width, -(float)e.height, 0.0f, 1.0f};
        VkRect2D scissor = {{(int32_t)e.viewportX, (int32_t)e.viewportY}, {e.width, e.height}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        float vp[16];
        mat4_multiply(vp, e.projMat, e.viewMat);

        // Draw textured cube
        {
            const float cubeSize = 0.3f;
            const float cubeHeight = 1.6f;

            float rot[16], scl[16], trans[16], model[16], tmp[16];
            mat4_rotation_y(rot, renderer.cubeRotation);
            mat4_scaling(scl, cubeSize, cubeSize, cubeSize);
            mat4_translation(trans, 0.0f, cubeHeight, -2.0f);
            mat4_multiply(tmp, scl, rot);
            mat4_multiply(model, trans, tmp);
            float mvp[16];
            mat4_multiply(mvp, vp, model);

            // Push constants: MVP (64 bytes) + model (64 bytes) = 128 bytes
            float pushData[32];
            memcpy(pushData, mvp, 64);
            memcpy(pushData + 16, model, 64);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.cubePipeline);
            vkCmdPushConstants(cmd, renderer.cubePipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT, 0, 128, pushData);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.cubePipelineLayout,
                0, 1, &renderer.descriptorSet, 0, nullptr);

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &renderer.cubeVertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, renderer.cubeIndexBuffer, 0, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(cmd, 36, 1, 0, 0, 0);
        }

        // Draw grid floor
        {
            const float gridScale = 0.05f;
            float scale[16], trans[16];
            mat4_scaling(scale, gridScale, gridScale, gridScale);
            mat4_translation(trans, 0.0f, gridScale, 0.0f);

            float tmp1[16], tmp2[16], mvp[16];
            mat4_multiply(tmp1, trans, scale);
            mat4_multiply(mvp, vp, tmp1);

            PushConstants pc = {};
            memcpy(pc.transform, mvp, sizeof(mvp));
            pc.color[0] = 0.3f; pc.color[1] = 0.3f; pc.color[2] = 0.35f; pc.color[3] = 1.0f;

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.gridPipeline);
            vkCmdPushConstants(cmd, renderer.pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &renderer.gridVertexBuffer, &offset);
            vkCmdDraw(cmd, renderer.gridVertexCount, 1, 0, 0);
        }
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    // Submit
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(renderer.graphicsQueue, 1, &submitInfo, renderer.frameFence);

    // Wait for completion before returning (runtime needs the image ready)
    vkWaitForFences(device, 1, &renderer.frameFence, VK_TRUE, UINT64_MAX);
}

static void CleanupVkRenderer(VkRenderer& renderer) {
    if (!renderer.device) return;

    vkDeviceWaitIdle(renderer.device);

    for (auto fb : renderer.framebuffers)
        vkDestroyFramebuffer(renderer.device, fb, nullptr);
    renderer.framebuffers.clear();

    for (auto iv : renderer.swapchainImageViews)
        vkDestroyImageView(renderer.device, iv, nullptr);
    renderer.swapchainImageViews.clear();

    if (renderer.depthView) {
        vkDestroyImageView(renderer.device, renderer.depthView, nullptr);
        renderer.depthView = VK_NULL_HANDLE;
    }
    if (renderer.depthImage) {
        vkDestroyImage(renderer.device, renderer.depthImage, nullptr);
        renderer.depthImage = VK_NULL_HANDLE;
    }
    if (renderer.depthMemory) {
        vkFreeMemory(renderer.device, renderer.depthMemory, nullptr);
        renderer.depthMemory = VK_NULL_HANDLE;
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
    // Texture cleanup
    if (renderer.descriptorPool) {
        vkDestroyDescriptorPool(renderer.device, renderer.descriptorPool, nullptr);
        renderer.descriptorPool = VK_NULL_HANDLE;
    }
    if (renderer.descriptorSetLayout) {
        vkDestroyDescriptorSetLayout(renderer.device, renderer.descriptorSetLayout, nullptr);
        renderer.descriptorSetLayout = VK_NULL_HANDLE;
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
    if (renderer.cubePipelineLayout) {
        vkDestroyPipelineLayout(renderer.device, renderer.cubePipelineLayout, nullptr);
        renderer.cubePipelineLayout = VK_NULL_HANDLE;
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

// ============================================================================
// OpenXR session management
// ============================================================================

struct SwapchainInfo {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t imageCount = 0;
};

struct AppXrSession {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;

    SwapchainInfo swapchain;

    XrViewConfigurationType viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    std::vector<XrViewConfigurationView> configViews;

    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;
    bool exitRequested = false;

    // Per-view dimensions from recommendedImageRectWidth/Height (set once at init)
    uint32_t viewWidth = 0;
    uint32_t viewHeight = 0;
};

static bool InitializeOpenXR(AppXrSession& xr) {
    LOG_INFO("Initializing OpenXR...");

    uint32_t extensionCount = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));

    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()));

    bool hasVulkan = false;
    for (const auto& ext : extensions) {
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) {
            hasVulkan = true;
        }
    }

    LOG_INFO("XR_KHR_vulkan_enable: %s", hasVulkan ? "AVAILABLE" : "NOT FOUND");
    if (!hasVulkan) {
        LOG_ERROR("XR_KHR_vulkan_enable extension not available");
        return false;
    }

    // Legacy: NOT enabling XR_EXT_display_info
    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(createInfo.applicationInfo.applicationName, "SimCubeOpenXR",
            sizeof(createInfo.applicationInfo.applicationName) - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    strncpy(createInfo.applicationInfo.engineName, "None",
            sizeof(createInfo.applicationInfo.engineName) - 1);
    createInfo.applicationInfo.engineVersion = 0;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    createInfo.enabledExtensionNames = enabledExtensions.data();

    XR_CHECK(xrCreateInstance(&createInfo, &xr.instance));
    LOG_INFO("OpenXR instance created");

    XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(xr.instance, &systemInfo, &xr.systemId));

    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr));
    xr.configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount, xr.configViews.data()));

    for (uint32_t i = 0; i < viewCount; i++) {
        LOG_INFO("  View %u: %ux%u", i,
            xr.configViews[i].recommendedImageRectWidth,
            xr.configViews[i].recommendedImageRectHeight);
    }

    // Store per-view dimensions from recommended (fixed for session lifetime)
    xr.viewWidth = xr.configViews[0].recommendedImageRectWidth;
    xr.viewHeight = xr.configViews[0].recommendedImageRectHeight;

    return true;
}

static bool GetVulkanGraphicsRequirements(AppXrSession& xr) {
    PFN_xrGetVulkanGraphicsRequirementsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&pfn));

    XrGraphicsRequirementsVulkanKHR graphicsReq = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    XR_CHECK(pfn(xr.instance, xr.systemId, &graphicsReq));

    LOG_INFO("Vulkan graphics requirements: %d.%d.%d - %d.%d.%d",
        VK_VERSION_MAJOR(graphicsReq.minApiVersionSupported),
        VK_VERSION_MINOR(graphicsReq.minApiVersionSupported),
        VK_VERSION_PATCH(graphicsReq.minApiVersionSupported),
        VK_VERSION_MAJOR(graphicsReq.maxApiVersionSupported),
        VK_VERSION_MINOR(graphicsReq.maxApiVersionSupported),
        VK_VERSION_PATCH(graphicsReq.maxApiVersionSupported));

    return true;
}

static bool CreateVulkanInstance(AppXrSession& xr, VkInstance& vkInstance) {
    LOG_INFO("Creating Vulkan instance...");

    // Get required instance extensions from the runtime
    PFN_xrGetVulkanInstanceExtensionsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanInstanceExtensionsKHR",
        (PFN_xrVoidFunction*)&pfn));

    uint32_t bufferSize = 0;
    pfn(xr.instance, xr.systemId, 0, &bufferSize, nullptr);
    std::string extensionsStr(bufferSize, '\0');
    pfn(xr.instance, xr.systemId, bufferSize, &bufferSize, extensionsStr.data());

    // Parse space-separated extension names
    std::vector<std::string> extensionNames;
    {
        size_t start = 0;
        while (start < extensionsStr.size()) {
            size_t end = extensionsStr.find(' ', start);
            if (end == std::string::npos) end = extensionsStr.size();
            std::string name = extensionsStr.substr(start, end - start);
            if (!name.empty() && name[0] != '\0') {
                extensionNames.push_back(name);
            }
            start = end + 1;
        }
    }

    // MoltenVK portability: enumerate available instance extensions and add
    // VK_KHR_portability_enumeration if present
    uint32_t availExtCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &availExtCount, nullptr);
    std::vector<VkExtensionProperties> availExts(availExtCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &availExtCount, availExts.data());

    bool hasPortabilityEnum = false;
    for (const auto& ext : availExts) {
        if (strcmp(ext.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
            hasPortabilityEnum = true;
            break;
        }
    }
    if (hasPortabilityEnum) {
        extensionNames.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        LOG_INFO("  Adding VK_KHR_portability_enumeration for MoltenVK");
    }

    std::vector<const char*> extensionPtrs;
    for (auto& name : extensionNames) {
        extensionPtrs.push_back(name.c_str());
        LOG_INFO("  VkInstance extension: %s", name.c_str());
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "SimCubeOpenXR";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "None";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = (uint32_t)extensionPtrs.size();
    createInfo.ppEnabledExtensionNames = extensionPtrs.data();
    if (hasPortabilityEnum) {
        createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VK_CHECK(vkCreateInstance(&createInfo, nullptr, &vkInstance));
    LOG_INFO("Vulkan instance created");
    return true;
}

static bool GetVulkanPhysicalDevice(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice) {
    PFN_xrGetVulkanGraphicsDeviceKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsDeviceKHR",
        (PFN_xrVoidFunction*)&pfn));

    XR_CHECK(pfn(xr.instance, xr.systemId, vkInstance, &physDevice));

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physDevice, &props);
    LOG_INFO("Vulkan physical device: %s", props.deviceName);

    return true;
}

static bool GetVulkanDeviceExtensions(AppXrSession& xr,
    std::vector<const char*>& deviceExtensions, std::vector<std::string>& extensionStorage,
    VkPhysicalDevice physDevice)
{
    PFN_xrGetVulkanDeviceExtensionsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanDeviceExtensionsKHR",
        (PFN_xrVoidFunction*)&pfn));

    uint32_t bufferSize = 0;
    pfn(xr.instance, xr.systemId, 0, &bufferSize, nullptr);

    std::string extensionsStr(bufferSize, '\0');
    pfn(xr.instance, xr.systemId, bufferSize, &bufferSize, extensionsStr.data());

    extensionStorage.clear();
    deviceExtensions.clear();
    {
        size_t start = 0;
        while (start < extensionsStr.size()) {
            size_t end = extensionsStr.find(' ', start);
            if (end == std::string::npos) end = extensionsStr.size();
            std::string name = extensionsStr.substr(start, end - start);
            if (!name.empty() && name[0] != '\0') {
                extensionStorage.push_back(name);
            }
            start = end + 1;
        }
    }

    // MoltenVK portability: add VK_KHR_portability_subset if available on device
    uint32_t devExtCount = 0;
    vkEnumerateDeviceExtensionProperties(physDevice, nullptr, &devExtCount, nullptr);
    std::vector<VkExtensionProperties> devExts(devExtCount);
    vkEnumerateDeviceExtensionProperties(physDevice, nullptr, &devExtCount, devExts.data());

    for (const auto& ext : devExts) {
        if (strcmp(ext.extensionName, "VK_KHR_portability_subset") == 0) {
            extensionStorage.push_back("VK_KHR_portability_subset");
            LOG_INFO("  Adding VK_KHR_portability_subset for MoltenVK");
            break;
        }
    }

    for (auto& name : extensionStorage) {
        deviceExtensions.push_back(name.c_str());
        LOG_INFO("  VkDevice extension: %s", name.c_str());
    }

    return true;
}

static bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex) {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queueFamilyIndex = i;
            LOG_INFO("Graphics queue family: %u", i);
            return true;
        }
    }

    LOG_ERROR("No graphics queue family found");
    return false;
}

static bool CreateVulkanDevice(VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    const std::vector<const char*>& deviceExtensions,
    VkDevice& device, VkQueue& graphicsQueue)
{
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamilyIndex;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = (uint32_t)deviceExtensions.size();
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    VK_CHECK(vkCreateDevice(physDevice, &createInfo, nullptr, &device));

    vkGetDeviceQueue(device, queueFamilyIndex, 0, &graphicsQueue);
    LOG_INFO("Vulkan device and graphics queue created");
    return true;
}

static bool CreateSession(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex)
{
    LOG_INFO("Creating OpenXR session with Vulkan binding...");

    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = vkInstance;
    vkBinding.physicalDevice = physDevice;
    vkBinding.device = device;
    vkBinding.queueFamilyIndex = queueFamilyIndex;
    vkBinding.queueIndex = 0;

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &vkBinding;
    sessionInfo.systemId = xr.systemId;

    XR_CHECK(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
    LOG_INFO("Session created");

    // Legacy: no rendering mode enumeration (XR_EXT_display_info not enabled)

    return true;
}

static bool CreateSpaces(AppXrSession& xr) {
    LOG_INFO("Creating reference spaces...");

    XrReferenceSpaceCreateInfo localSpaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    localSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    localSpaceInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
    localSpaceInfo.poseInReferenceSpace.position = {0, 0, 0};
    XR_CHECK(xrCreateReferenceSpace(xr.session, &localSpaceInfo, &xr.localSpace));

    XrReferenceSpaceCreateInfo viewSpaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    viewSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewSpaceInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
    viewSpaceInfo.poseInReferenceSpace.position = {0, 0, 0};
    XR_CHECK(xrCreateReferenceSpace(xr.session, &viewSpaceInfo, &xr.viewSpace));

    LOG_INFO("Reference spaces created (LOCAL + VIEW)");
    return true;
}

static bool CreateSwapchain(AppXrSession& xr) {
    LOG_INFO("Creating atlas swapchain...");

    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, 0, &formatCount, nullptr));

    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, formatCount, &formatCount, formats.data()));

    int64_t selectedFormat = formats[0];
    LOG_INFO("Selected swapchain format: %lld (0x%llX)", (long long)selectedFormat, (long long)selectedFormat);

    const auto& view = xr.configViews[0];

    // Legacy: use recommendedImageRectWidth * 2 (stereo SBS) since no modes are enumerated
    uint32_t scWidth = view.recommendedImageRectWidth * 2;
    uint32_t scHeight = view.recommendedImageRectHeight;

    XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swapchainInfo.format = selectedFormat;
    swapchainInfo.sampleCount = view.recommendedSwapchainSampleCount;
    swapchainInfo.width = scWidth;
    swapchainInfo.height = scHeight;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;

    LOG_INFO("  Atlas swapchain: %ux%u", swapchainInfo.width, swapchainInfo.height);

    XR_CHECK(xrCreateSwapchain(xr.session, &swapchainInfo, &xr.swapchain.swapchain));

    xr.swapchain.format = selectedFormat;
    xr.swapchain.width = swapchainInfo.width;
    xr.swapchain.height = swapchainInfo.height;

    uint32_t imageCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(xr.swapchain.swapchain, 0, &imageCount, nullptr));
    xr.swapchain.imageCount = imageCount;

    LOG_INFO("  %u swapchain images", imageCount);
    LOG_INFO("Atlas swapchain created");
    return true;
}

static bool PollEvents(AppXrSession& xr) {
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};

    while (xrPollEvent(xr.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* stateEvent = (XrEventDataSessionStateChanged*)&event;
            xr.sessionState = stateEvent->state;

            switch (xr.sessionState) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = xr.viewConfigType;
                if (XR_SUCCEEDED(xrBeginSession(xr.session, &beginInfo))) {
                    xr.sessionRunning = true;
                    LOG_INFO("Session running");
                }
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                xrEndSession(xr.session);
                xr.sessionRunning = false;
                LOG_INFO("Session stopped");
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                xr.exitRequested = true;
                break;
            default:
                break;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            xr.exitRequested = true;
            break;
        // Legacy app: no rendering mode events (XR_EXT_display_info not enabled)
        default:
            break;
        }

        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }

    return true;
}

static bool BeginFrame(AppXrSession& xr, XrFrameState& frameState) {
    frameState = {XR_TYPE_FRAME_STATE};

    XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
    XrResult result = xrWaitFrame(xr.session, &waitInfo, &frameState);
    if (XR_FAILED(result)) {
        xr.exitRequested = true;
        return false;
    }

    XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
    result = xrBeginFrame(xr.session, &beginInfo);
    if (XR_FAILED(result)) return false;

    return true;
}

static bool AcquireSwapchainImage(AppXrSession& xr, uint32_t& imageIndex) {
    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult result = xrAcquireSwapchainImage(xr.swapchain.swapchain, &acquireInfo, &imageIndex);
    if (XR_FAILED(result)) return false;

    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(xr.swapchain.swapchain, &waitInfo);
    if (XR_FAILED(result)) {
        XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(xr.swapchain.swapchain, &releaseInfo);
        return false;
    }

    return true;
}

static bool ReleaseSwapchainImage(AppXrSession& xr) {
    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return XR_SUCCEEDED(xrReleaseSwapchainImage(xr.swapchain.swapchain, &releaseInfo));
}

static bool EndFrame(AppXrSession& xr, XrTime displayTime, const XrCompositionLayerProjectionView* views, uint32_t viewCount) {
    XrCompositionLayerProjection projectionLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projectionLayer.space = xr.localSpace;
    projectionLayer.viewCount = viewCount;
    projectionLayer.views = views;

    const XrCompositionLayerBaseHeader* layers[] = {
        (XrCompositionLayerBaseHeader*)&projectionLayer
    };

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = displayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = 1;
    endInfo.layers = layers;

    return XR_SUCCEEDED(xrEndFrame(xr.session, &endInfo));
}

static void CleanupOpenXR(AppXrSession& xr) {
    if (xr.swapchain.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(xr.swapchain.swapchain);
        xr.swapchain.swapchain = XR_NULL_HANDLE;
    }
    if (xr.viewSpace != XR_NULL_HANDLE) {
        xrDestroySpace(xr.viewSpace);
        xr.viewSpace = XR_NULL_HANDLE;
    }
    if (xr.localSpace != XR_NULL_HANDLE) {
        xrDestroySpace(xr.localSpace);
        xr.localSpace = XR_NULL_HANDLE;
    }
    if (xr.session != XR_NULL_HANDLE) {
        xrDestroySession(xr.session);
        xr.session = XR_NULL_HANDLE;
    }
    if (xr.instance != XR_NULL_HANDLE) {
        xrDestroyInstance(xr.instance);
        xr.instance = XR_NULL_HANDLE;
    }
    LOG_INFO("OpenXR cleanup complete");
}

// ============================================================================
// Main
// ============================================================================

static volatile bool g_running = true;

static void SignalHandler(int sig) {
    (void)sig;
    g_running = false;
}

int main() {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    LOG_INFO("=== Sim Cube OpenXR (Vulkan) ===");

    // Initialize OpenXR
    AppXrSession xr = {};
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        return 1;
    }

    if (!GetVulkanGraphicsRequirements(xr)) {
        LOG_ERROR("Failed to get Vulkan graphics requirements");
        CleanupOpenXR(xr);
        return 1;
    }

    // Create Vulkan instance
    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) {
        LOG_ERROR("Vulkan instance creation failed");
        CleanupOpenXR(xr);
        return 1;
    }

    // Get physical device
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        LOG_ERROR("Failed to get Vulkan physical device");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    // Get device extensions
    std::vector<const char*> deviceExtensions;
    std::vector<std::string> extensionStorage;
    if (!GetVulkanDeviceExtensions(xr, deviceExtensions, extensionStorage, physDevice)) {
        LOG_ERROR("Failed to get Vulkan device extensions");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    // Enable VK_KHR_maintenance1 for negative viewport height (Y-flip).
    // Without this extension, negative viewport height is undefined in Vulkan 1.0.
    deviceExtensions.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);

    // Find graphics queue
    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    // Create logical device
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, deviceExtensions, vkDevice, graphicsQueue)) {
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    // Create session
    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex)) {
        LOG_ERROR("Session creation failed");
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    if (!CreateSpaces(xr)) {
        LOG_ERROR("Space creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        return 1;
    }

    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        return 1;
    }

    // Enumerate Vulkan swapchain images
    uint32_t scImageCount = xr.swapchain.imageCount;
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages(scImageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(xr.swapchain.swapchain, scImageCount, &scImageCount,
        (XrSwapchainImageBaseHeader*)swapchainImages.data());
    LOG_INFO("Enumerated %u Vulkan swapchain images", scImageCount);

    // Initialize Vulkan renderer
    VkFormat colorFormat = (VkFormat)xr.swapchain.format;
    VkRenderer vkRenderer = {};
    if (!InitializeVkRenderer(vkRenderer, vkDevice, physDevice, graphicsQueue, queueFamilyIndex, colorFormat)) {
        LOG_ERROR("Vulkan renderer initialization failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        return 1;
    }

    // Create framebuffers for the single SBS swapchain
    {
        std::vector<VkImage> images(scImageCount);
        for (uint32_t i = 0; i < scImageCount; i++) {
            images[i] = swapchainImages[i].image;
        }

        if (!CreateSwapchainFramebuffers(vkRenderer, images.data(), scImageCount,
            xr.swapchain.width, xr.swapchain.height, colorFormat)) {
            LOG_ERROR("Failed to create framebuffers");
            CleanupVkRenderer(vkRenderer);
            CleanupOpenXR(xr);
            vkDestroyDevice(vkDevice, nullptr);
            vkDestroyInstance(vkInstance, nullptr);
            return 1;
        }
    }

    LOG_INFO("=== Entering main loop (Ctrl+C to exit) ===");

    // Frame timing
    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !xr.exitRequested) {
        // Delta time
        auto now = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        // Update scene
        vkRenderer.cubeRotation += deltaTime * 0.5f;
        if (vkRenderer.cubeRotation > 2.0f * 3.14159265f) {
            vkRenderer.cubeRotation -= 2.0f * 3.14159265f;
        }

        PollEvents(xr);

        if (xr.sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(xr, frameState)) {
                bool rendered = false;
                uint32_t viewCount = 0;
                std::vector<XrCompositionLayerProjectionView> projectionViews;

                if (frameState.shouldRender) {
                    // Locate views — first query count, then fetch
                    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                    locateInfo.viewConfigurationType = xr.viewConfigType;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = xr.localSpace;

                    XrViewState viewState = {XR_TYPE_VIEW_STATE};
                    // Query view count
                    xrLocateViews(xr.session, &locateInfo, &viewState, 0, &viewCount, nullptr);
                    if (viewCount == 0) viewCount = 2; // fallback

                    std::vector<XrView> views(viewCount, {XR_TYPE_VIEW});
                    XrResult locResult = xrLocateViews(xr.session, &locateInfo, &viewState, viewCount, &viewCount, views.data());
                    if (XR_SUCCEEDED(locResult) &&
                        (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
                    {
                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(xr, imageIndex)) {
                            rendered = true;

                            // Legacy app: always render 2 stereo views in SBS layout.
                            // View dimensions come from recommendedImageRectWidth/Height (set at init).
                            // The runtime handles 2D/3D mode switching — we just render the same SBS atlas.
                            uint32_t eyeCount = 2;
                            uint32_t eyeW = xr.viewWidth;
                            uint32_t eyeH = xr.viewHeight;

                            EyeRenderParams eyeParams[2];
                            projectionViews.resize(eyeCount, {});
                            for (uint32_t i = 0; i < eyeCount; i++) {
                                eyeParams[i].viewportX = i * eyeW;  // SBS: left eye at 0, right at eyeW
                                eyeParams[i].viewportY = 0;
                                eyeParams[i].width = eyeW;
                                eyeParams[i].height = eyeH;
                                mat4_view_from_xr_pose(eyeParams[i].viewMat, views[i].pose);
                                mat4_from_xr_fov(eyeParams[i].projMat, views[i].fov, 0.01f, 100.0f);

                                projectionViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[i].subImage.swapchain = xr.swapchain.swapchain;
                                projectionViews[i].subImage.imageRect.offset = {(int32_t)(i * eyeW), 0};
                                projectionViews[i].subImage.imageRect.extent = {(int32_t)eyeW, (int32_t)eyeH};
                                projectionViews[i].subImage.imageArrayIndex = 0;
                                projectionViews[i].pose = views[i].pose;
                                projectionViews[i].fov = views[i].fov;
                            }

                            RenderScene(vkRenderer, imageIndex, eyeParams, (int)eyeCount);
                            ReleaseSwapchainImage(xr);
                        }
                    }
                }

                if (rendered) {
                    EndFrame(xr, frameState.predictedDisplayTime, projectionViews.data(), (uint32_t)projectionViews.size());
                } else {
                    // Submit empty frame
                    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                    endInfo.displayTime = frameState.predictedDisplayTime;
                    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    endInfo.layerCount = 0;
                    endInfo.layers = nullptr;
                    xrEndFrame(xr.session, &endInfo);
                }
            }
        } else {
            // Session not running — sleep to avoid busy-waiting
#ifdef _WIN32
            Sleep(100);
#else
            usleep(100000);
#endif
        }
    }

    // Request exit if we got SIGINT but session is still running
    if (!xr.exitRequested && xr.session != XR_NULL_HANDLE && xr.sessionRunning) {
        LOG_INFO("Requesting session exit...");
        xrRequestExitSession(xr.session);
        // Drain events until exit completes
        for (int i = 0; i < 100 && !xr.exitRequested; i++) {
            PollEvents(xr);
#ifdef _WIN32
            Sleep(10);
#else
            usleep(10000);
#endif
        }
    }

    LOG_INFO("=== Shutting down ===");

    CleanupVkRenderer(vkRenderer);
    CleanupOpenXR(xr);
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(vkInstance, nullptr);

    LOG_INFO("Application shutdown complete");
    return 0;
}
