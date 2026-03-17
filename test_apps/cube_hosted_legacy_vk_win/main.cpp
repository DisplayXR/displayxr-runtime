// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR - Standard OpenXR mode with Vulkan (DisplayXR creates window)
 *
 * No windowing code — the runtime's compositor creates its own window.
 * No input handling — static camera with continuous cube rotation.
 * Input is handled by DisplayXR's qwerty driver.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "logging.h"
#include "xr_session.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

static const char* APP_NAME = "cube_hosted_legacy_vk_win";
static bool g_running = true;

// ============================================================================
// Performance tracking
// ============================================================================

struct PerformanceStats {
    std::chrono::high_resolution_clock::time_point lastTime;
    float deltaTime = 0.0f;
    float fps = 0.0f;
    float frameTimeMs = 0.0f;
    int frameCount = 0;
    float fpsAccumulator = 0.0f;
};

static void UpdatePerformanceStats(PerformanceStats& stats) {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - stats.lastTime);
    stats.deltaTime = duration.count() / 1000000.0f;
    stats.frameTimeMs = duration.count() / 1000.0f;
    stats.lastTime = now;

    stats.fpsAccumulator += stats.deltaTime;
    stats.frameCount++;

    if (stats.fpsAccumulator >= 1.0f) {
        stats.fps = stats.frameCount / stats.fpsAccumulator;
        stats.frameCount = 0;
        stats.fpsAccumulator = 0.0f;
    }
}

// ============================================================================
// Inline math — column-major float[16] matrices
// ============================================================================

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
    m[12] = tx; m[13] = ty; m[14] = tz;
}

static void mat4_scaling(float* m, float sx, float sy, float sz) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = sx; m[5] = sy; m[10] = sz; m[15] = 1.0f;
}

static void mat4_rotation_y(float* m, float angle) {
    float c = cosf(angle), s = sinf(angle);
    mat4_identity(m);
    m[0] = c; m[8] = s; m[2] = -s; m[10] = c;
}

static void mat4_from_xr_fov(float* m, const XrFovf& fov, float nearZ, float farZ) {
    float left   = nearZ * tanf(fov.angleLeft);
    float right  = nearZ * tanf(fov.angleRight);
    float top    = nearZ * tanf(fov.angleUp);
    float bottom = nearZ * tanf(fov.angleDown);
    float w = right - left;
    float h = top - bottom;

    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f * nearZ / w;
    m[5]  = 2.0f * nearZ / h;
    m[8]  = (right + left) / w;
    m[9]  = (top + bottom) / h;
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -2.0f * farZ * nearZ / (farZ - nearZ);
}

static void mat4_view_from_xr_pose(float* m, const XrPosef& pose) {
    float qx = pose.orientation.x, qy = pose.orientation.y;
    float qz = pose.orientation.z, qw = pose.orientation.w;

    float r00 = 1.0f - 2.0f*(qy*qy + qz*qz);
    float r01 = 2.0f*(qx*qy - qz*qw);
    float r02 = 2.0f*(qx*qz + qy*qw);
    float r10 = 2.0f*(qx*qy + qz*qw);
    float r11 = 1.0f - 2.0f*(qx*qx + qz*qz);
    float r12 = 2.0f*(qy*qz - qx*qw);
    float r20 = 2.0f*(qx*qz - qy*qw);
    float r21 = 2.0f*(qy*qz + qx*qw);
    float r22 = 1.0f - 2.0f*(qx*qx + qy*qy);

    float px = pose.position.x, py = pose.position.y, pz = pose.position.z;
    float tx = -(r00*px + r10*py + r20*pz);
    float ty = -(r01*px + r11*py + r21*pz);
    float tz = -(r02*px + r12*py + r22*pz);

    m[0]=r00; m[4]=r10; m[8] =r20; m[12]=tx;
    m[1]=r01; m[5]=r11; m[9] =r21; m[13]=ty;
    m[2]=r02; m[6]=r12; m[10]=r22; m[14]=tz;
    m[3]=0.0f;m[7]=0.0f;m[11]=0.0f;m[15]=1.0f;
}

// ============================================================================
// Texture path helper
// ============================================================================

static std::string GetTextureDir() {
    char pathBuf[4096];
    DWORD len = GetModuleFileNameA(NULL, pathBuf, sizeof(pathBuf));
    if (len > 0 && len < sizeof(pathBuf)) {
        char* lastSlash = strrchr(pathBuf, '\\');
        if (lastSlash) *lastSlash = '\0';
        return std::string(pathBuf) + "\\textures\\";
    }
    return "textures\\";
}

// ============================================================================
// Embedded SPIR-V shaders
// ============================================================================

// Textured cube vertex shader
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

// Textured cube fragment shader
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

// Grid vertex shader
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

// Grid fragment shader
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
// Vulkan helpers
// ============================================================================

static uint32_t FindMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
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
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) return false;

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
    if (vkCreateShaderModule(device, &createInfo, nullptr, &module) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module;
}

static bool CreateTextureFromFile(VkDevice device, VkPhysicalDevice physDevice,
    VkCommandPool cmdPool, VkQueue queue,
    const char* path, bool isNormalMap,
    VkImage& outImage, VkDeviceMemory& outMemory, VkImageView& outView)
{
    int w, h, channels;
    unsigned char* pixels = stbi_load(path, &w, &h, &channels, 4);
    bool fallback = false;
    if (!pixels) {
        LOG_WARN("Failed to load texture: %s — using fallback", path);
        w = h = 1;
        static unsigned char whitePixel[] = {255, 255, 255, 255};
        static unsigned char bluePixel[] = {128, 128, 255, 255};
        pixels = isNormalMap ? bluePixel : whitePixel;
        fallback = true;
    }

    VkDeviceSize imageSize = (VkDeviceSize)w * h * 4;

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

    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);

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
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout cubePipelineLayout = VK_NULL_HANDLE;
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

    VkImage texImages[3] = {};
    VkDeviceMemory texMemory[3] = {};
    VkImageView texViews[3] = {};
    VkSampler texSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    bool texturesLoaded = false;
};

// ============================================================================
// Renderer init
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
        VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

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

    // Grid pipeline layout (push constants only, 80 bytes)
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

    // Cube pipeline layout (descriptor set + 128 bytes push constants)
    {
        VkPushConstantRange cubePushRange = {};
        cubePushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        cubePushRange.offset = 0;
        cubePushRange.size = 128;

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

    // Common pipeline state helpers
    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

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
        binding.stride = sizeof(CubeVertex);

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

        VkPipelineRasterizationStateCreateInfo rasterizer = {};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

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

        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &renderer.cubePipeline) != VK_SUCCESS) {
            LOG_ERROR("Failed to create cube pipeline");
            return false;
        }
    }

    // Grid pipeline (line list)
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
        binding.stride = sizeof(GridVertex);

        VkVertexInputAttributeDescription attr = {};
        attr.format = VK_FORMAT_R32G32B32_SFLOAT;

        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions = &attr;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

        VkPipelineRasterizationStateCreateInfo rasterizer = {};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_NONE;

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

        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &renderer.gridPipeline) != VK_SUCCESS) {
            LOG_ERROR("Failed to create grid pipeline");
            return false;
        }
    }

    vkDestroyShaderModule(device, cubeVert, nullptr);
    vkDestroyShaderModule(device, cubeFrag, nullptr);
    vkDestroyShaderModule(device, gridVert, nullptr);
    vkDestroyShaderModule(device, gridFrag, nullptr);

    // Cube vertex/index buffers
    CubeVertex cubeVerts[] = {
        {{-0.5f,-0.5f,-0.5f},{1,1,1,1},{0,1},{0,0,-1},{1,0,0}},
        {{-0.5f, 0.5f,-0.5f},{1,1,1,1},{0,0},{0,0,-1},{1,0,0}},
        {{ 0.5f, 0.5f,-0.5f},{1,1,1,1},{1,0},{0,0,-1},{1,0,0}},
        {{ 0.5f,-0.5f,-0.5f},{1,1,1,1},{1,1},{0,0,-1},{1,0,0}},
        {{-0.5f,-0.5f, 0.5f},{1,1,1,1},{1,1},{0,0,1},{-1,0,0}},
        {{ 0.5f,-0.5f, 0.5f},{1,1,1,1},{0,1},{0,0,1},{-1,0,0}},
        {{ 0.5f, 0.5f, 0.5f},{1,1,1,1},{0,0},{0,0,1},{-1,0,0}},
        {{-0.5f, 0.5f, 0.5f},{1,1,1,1},{1,0},{0,0,1},{-1,0,0}},
        {{-0.5f, 0.5f,-0.5f},{1,1,1,1},{0,1},{0,1,0},{1,0,0}},
        {{-0.5f, 0.5f, 0.5f},{1,1,1,1},{0,0},{0,1,0},{1,0,0}},
        {{ 0.5f, 0.5f, 0.5f},{1,1,1,1},{1,0},{0,1,0},{1,0,0}},
        {{ 0.5f, 0.5f,-0.5f},{1,1,1,1},{1,1},{0,1,0},{1,0,0}},
        {{-0.5f,-0.5f,-0.5f},{1,1,1,1},{0,0},{0,-1,0},{1,0,0}},
        {{ 0.5f,-0.5f,-0.5f},{1,1,1,1},{1,0},{0,-1,0},{1,0,0}},
        {{ 0.5f,-0.5f, 0.5f},{1,1,1,1},{1,1},{0,-1,0},{1,0,0}},
        {{-0.5f,-0.5f, 0.5f},{1,1,1,1},{0,1},{0,-1,0},{1,0,0}},
        {{-0.5f,-0.5f, 0.5f},{1,1,1,1},{0,1},{-1,0,0},{0,0,-1}},
        {{-0.5f, 0.5f, 0.5f},{1,1,1,1},{0,0},{-1,0,0},{0,0,-1}},
        {{-0.5f, 0.5f,-0.5f},{1,1,1,1},{1,0},{-1,0,0},{0,0,-1}},
        {{-0.5f,-0.5f,-0.5f},{1,1,1,1},{1,1},{-1,0,0},{0,0,-1}},
        {{ 0.5f,-0.5f,-0.5f},{1,1,1,1},{0,1},{1,0,0},{0,0,1}},
        {{ 0.5f, 0.5f,-0.5f},{1,1,1,1},{0,0},{1,0,0},{0,0,1}},
        {{ 0.5f, 0.5f, 0.5f},{1,1,1,1},{1,0},{1,0,0},{0,0,1}},
        {{ 0.5f,-0.5f, 0.5f},{1,1,1,1},{1,1},{1,0,0},{0,0,1}},
    };

    uint16_t cubeIndices[] = {
        0,1,2, 0,2,3, 4,5,6, 4,6,7, 8,9,10, 8,10,11,
        12,13,14, 12,14,15, 16,17,18, 16,18,19, 20,21,22, 20,22,23,
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

    // Command pool + buffer + fence
    {
        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &renderer.commandPool) != VK_SUCCESS) return false;

        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = renderer.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &allocInfo, &renderer.commandBuffer) != VK_SUCCESS) return false;

        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(device, &fenceInfo, nullptr, &renderer.frameFence) != VK_SUCCESS) return false;
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
                    renderer.texImages[i], renderer.texMemory[i], renderer.texViews[i]))
                renderer.texturesLoaded = false;
        }

        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.maxLod = 1.0f;
        if (vkCreateSampler(device, &samplerInfo, nullptr, &renderer.texSampler) != VK_SUCCESS) return false;

        VkDescriptorPoolSize poolSize = {};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 3;
        VkDescriptorPoolCreateInfo dpInfo = {};
        dpInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpInfo.maxSets = 1;
        dpInfo.poolSizeCount = 1;
        dpInfo.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(device, &dpInfo, nullptr, &renderer.descriptorPool) != VK_SUCCESS) return false;

        VkDescriptorSetAllocateInfo dsAllocInfo = {};
        dsAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsAllocInfo.descriptorPool = renderer.descriptorPool;
        dsAllocInfo.descriptorSetCount = 1;
        dsAllocInfo.pSetLayouts = &renderer.descriptorSetLayout;
        if (vkAllocateDescriptorSets(device, &dsAllocInfo, &renderer.descriptorSet) != VK_SUCCESS) return false;

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
    }

    LOG_INFO("Vulkan renderer initialized");
    return true;
}

// ============================================================================
// Swapchain framebuffers
// ============================================================================

static bool CreateSwapchainFramebuffers(VkRenderer& renderer,
    const VkImage* images, uint32_t count,
    uint32_t width, uint32_t height, VkFormat colorFormat)
{
    VkDevice device = renderer.device;
    renderer.fbWidth = width;
    renderer.fbHeight = height;

    // Depth image
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
        if (vkCreateImage(device, &imageInfo, nullptr, &renderer.depthImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, renderer.depthImage, &memReqs);
        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(renderer.physicalDevice, memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &allocInfo, nullptr, &renderer.depthMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(device, renderer.depthImage, renderer.depthMemory, 0);

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = renderer.depthImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &viewInfo, nullptr, &renderer.depthView) != VK_SUCCESS) return false;
    }

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
        if (vkCreateImageView(device, &viewInfo, nullptr, &renderer.swapchainImageViews[i]) != VK_SUCCESS) return false;

        VkImageView attachments[] = { renderer.swapchainImageViews[i], renderer.depthView };
        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = renderer.renderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = width;
        fbInfo.height = height;
        fbInfo.layers = 1;
        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &renderer.framebuffers[i]) != VK_SUCCESS) return false;
    }

    LOG_INFO("Created %u framebuffers (%ux%u)", count, width, height);
    return true;
}

// ============================================================================
// Render scene
// ============================================================================

static void RenderScene(VkRenderer& renderer, uint32_t imageIndex,
    const EyeRenderParams* eyes, int eyeCount)
{
    VkDevice device = renderer.device;
    vkWaitForFences(device, 1, &renderer.frameFence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &renderer.frameFence);

    VkCommandBuffer cmd = renderer.commandBuffer;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

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

    for (int eye = 0; eye < eyeCount; eye++) {
        const auto& e = eyes[eye];

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
            float rot[16], scl[16], trans[16], model[16], tmp[16];
            mat4_rotation_y(rot, renderer.cubeRotation);
            mat4_scaling(scl, cubeSize, cubeSize, cubeSize);
            mat4_translation(trans, 0.0f, 1.6f, -2.0f);
            mat4_multiply(tmp, scl, rot);
            mat4_multiply(model, trans, tmp);
            float mvp[16];
            mat4_multiply(mvp, vp, model);

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

            float tmp1[16], mvp[16];
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

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(renderer.graphicsQueue, 1, &submitInfo, renderer.frameFence);
    vkWaitForFences(device, 1, &renderer.frameFence, VK_TRUE, UINT64_MAX);
}

// ============================================================================
// Cleanup
// ============================================================================

static void CleanupVkRenderer(VkRenderer& renderer) {
    if (!renderer.device) return;
    vkDeviceWaitIdle(renderer.device);

    for (auto fb : renderer.framebuffers) vkDestroyFramebuffer(renderer.device, fb, nullptr);
    for (auto iv : renderer.swapchainImageViews) vkDestroyImageView(renderer.device, iv, nullptr);

    if (renderer.depthView) vkDestroyImageView(renderer.device, renderer.depthView, nullptr);
    if (renderer.depthImage) vkDestroyImage(renderer.device, renderer.depthImage, nullptr);
    if (renderer.depthMemory) vkFreeMemory(renderer.device, renderer.depthMemory, nullptr);
    if (renderer.frameFence) vkDestroyFence(renderer.device, renderer.frameFence, nullptr);
    if (renderer.commandPool) vkDestroyCommandPool(renderer.device, renderer.commandPool, nullptr);
    if (renderer.gridVertexBuffer) vkDestroyBuffer(renderer.device, renderer.gridVertexBuffer, nullptr);
    if (renderer.gridVertexMemory) vkFreeMemory(renderer.device, renderer.gridVertexMemory, nullptr);
    if (renderer.cubeIndexBuffer) vkDestroyBuffer(renderer.device, renderer.cubeIndexBuffer, nullptr);
    if (renderer.cubeIndexMemory) vkFreeMemory(renderer.device, renderer.cubeIndexMemory, nullptr);
    if (renderer.cubeVertexBuffer) vkDestroyBuffer(renderer.device, renderer.cubeVertexBuffer, nullptr);
    if (renderer.cubeVertexMemory) vkFreeMemory(renderer.device, renderer.cubeVertexMemory, nullptr);
    if (renderer.gridPipeline) vkDestroyPipeline(renderer.device, renderer.gridPipeline, nullptr);
    if (renderer.cubePipeline) vkDestroyPipeline(renderer.device, renderer.cubePipeline, nullptr);
    if (renderer.descriptorPool) vkDestroyDescriptorPool(renderer.device, renderer.descriptorPool, nullptr);
    if (renderer.descriptorSetLayout) vkDestroyDescriptorSetLayout(renderer.device, renderer.descriptorSetLayout, nullptr);
    if (renderer.texSampler) vkDestroySampler(renderer.device, renderer.texSampler, nullptr);
    for (int i = 0; i < 3; i++) {
        if (renderer.texViews[i]) vkDestroyImageView(renderer.device, renderer.texViews[i], nullptr);
        if (renderer.texImages[i]) vkDestroyImage(renderer.device, renderer.texImages[i], nullptr);
        if (renderer.texMemory[i]) vkFreeMemory(renderer.device, renderer.texMemory[i], nullptr);
    }
    if (renderer.cubePipelineLayout) vkDestroyPipelineLayout(renderer.device, renderer.cubePipelineLayout, nullptr);
    if (renderer.pipelineLayout) vkDestroyPipelineLayout(renderer.device, renderer.pipelineLayout, nullptr);
    if (renderer.renderPass) vkDestroyRenderPass(renderer.device, renderer.renderPass, nullptr);
}

// ============================================================================
// WinMain
// ============================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR Cube OpenXR Vulkan Application ===");
    LOG_INFO("OpenXR standard mode (DisplayXR creates window)");
    LOG_INFO("Input handled by DisplayXR's qwerty driver");

    // Add DisplayXR to DLL search path
    {
        HKEY hKey;
        char installPath[MAX_PATH] = {0};
        DWORD pathSize = sizeof(installPath);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\DisplayXR\\Runtime", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr, (LPBYTE)installPath, &pathSize) == ERROR_SUCCESS) {
                LOG_INFO("Adding DisplayXR install path to DLL search: %s", installPath);
                SetDllDirectoryA(installPath);
            }
            RegCloseKey(hKey);
        }
    }

    // Initialize OpenXR
    XrSessionManager xr = {};
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        ShutdownLogging();
        return 1;
    }

    // Get Vulkan graphics requirements
    if (!GetVulkanGraphicsRequirements(xr)) {
        LOG_ERROR("Failed to get Vulkan graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create Vulkan instance
    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) {
        LOG_ERROR("Failed to create Vulkan instance");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Get physical device from runtime
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        LOG_ERROR("Failed to get Vulkan physical device");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Get device extensions
    std::vector<const char*> deviceExtensions;
    std::vector<std::string> extensionStorage;
    if (!GetVulkanDeviceExtensions(xr, vkInstance, physDevice, deviceExtensions, extensionStorage)) {
        LOG_ERROR("Failed to get Vulkan device extensions");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Find graphics queue family
    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        LOG_ERROR("Failed to find graphics queue family");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create logical device
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, deviceExtensions, vkDevice, graphicsQueue)) {
        LOG_ERROR("Failed to create Vulkan device");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create OpenXR session (DisplayXR creates window)
    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex, 0)) {
        LOG_ERROR("OpenXR session creation failed");
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    // Initialize Vulkan renderer
    VkRenderer renderer = {};
    VkFormat swapchainFormat = (VkFormat)xr.swapchain.format;
    if (!InitializeVkRenderer(renderer, vkDevice, physDevice, graphicsQueue, queueFamilyIndex, swapchainFormat)) {
        LOG_ERROR("Vulkan renderer initialization failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    // Enumerate Vulkan swapchain images
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u Vulkan swapchain images", count);

        std::vector<VkImage> images(count);
        for (uint32_t i = 0; i < count; i++) images[i] = swapchainImages[i].image;

        if (!CreateSwapchainFramebuffers(renderer, images.data(), count,
                xr.swapchain.width, xr.swapchain.height, swapchainFormat)) {
            LOG_ERROR("Failed to create swapchain framebuffers");
            CleanupVkRenderer(renderer);
            CleanupOpenXR(xr);
            vkDestroyDevice(vkDevice, nullptr);
            vkDestroyInstance(vkInstance, nullptr);
            ShutdownLogging();
            return 1;
        }
    }

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Rendering in DisplayXR's window (input via qwerty driver)");
    LOG_INFO("Controls: WASD=Move, QE=Up/Down, Mouse=Look, ESC=Quit");
    LOG_INFO("");

    while (g_running && !xr.exitRequested) {
        UpdatePerformanceStats(perfStats);
        renderer.cubeRotation += perfStats.deltaTime * 0.5f;

        PollEvents(xr);

        if (xr.sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(xr, frameState)) {
                XrCompositionLayerProjectionView projectionViews[8] = {};
                uint32_t submitViewCount = 2;

                if (frameState.shouldRender) {
                    // Locate views directly (column-major math, not XMMATRIX)
                    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                    locateInfo.viewConfigurationType = xr.viewConfigType;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = xr.localSpace;

                    XrViewState viewState = {XR_TYPE_VIEW_STATE};
                    uint32_t viewCount = 8;
                    XrView views[8];
                    for (uint32_t i = 0; i < 8; i++) views[i] = {XR_TYPE_VIEW};
                    xrLocateViews(xr.session, &locateInfo, &viewState, 8, &viewCount, views);
                    submitViewCount = viewCount;

                    // Get tile layout from rendering mode, with fallback
                    uint32_t tileColumns = (xr.currentModeIndex < xr.renderingModeCount)
                        ? xr.renderingModeTileColumns[xr.currentModeIndex] : (viewCount >= 2 ? 2 : 1);
                    uint32_t tileRows = (xr.currentModeIndex < xr.renderingModeCount)
                        ? xr.renderingModeTileRows[xr.currentModeIndex] : ((viewCount + tileColumns - 1) / tileColumns);

                    uint32_t tileW = xr.swapchain.width / tileColumns;
                    uint32_t tileH = xr.swapchain.height / tileRows;

                    uint32_t imageIndex;
                    if (AcquireSwapchainImage(xr, imageIndex)) {
                        EyeRenderParams eyeParams[8] = {};
                        for (uint32_t eye = 0; eye < viewCount; eye++) {
                            uint32_t tileX = eye % tileColumns;
                            uint32_t tileY = eye / tileColumns;

                            eyeParams[eye].viewportX = tileX * tileW;
                            eyeParams[eye].viewportY = tileY * tileH;
                            eyeParams[eye].width = tileW;
                            eyeParams[eye].height = tileH;
                            mat4_view_from_xr_pose(eyeParams[eye].viewMat, views[eye].pose);
                            mat4_from_xr_fov(eyeParams[eye].projMat, views[eye].fov, 0.05f, 100.0f);

                            projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                            projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                            projectionViews[eye].subImage.imageRect.offset = {(int32_t)(tileX * tileW), (int32_t)(tileY * tileH)};
                            projectionViews[eye].subImage.imageRect.extent = {(int32_t)tileW, (int32_t)tileH};
                            projectionViews[eye].subImage.imageArrayIndex = 0;
                            projectionViews[eye].pose = views[eye].pose;
                            projectionViews[eye].fov = views[eye].fov;
                        }

                        RenderScene(renderer, imageIndex, eyeParams, (int)viewCount);
                        ReleaseSwapchainImage(xr);
                    }
                }

                EndFrame(xr, frameState.predictedDisplayTime, projectionViews, submitViewCount);
            }
        } else {
            Sleep(100);
        }
    }

    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    vkDeviceWaitIdle(vkDevice);
    CleanupVkRenderer(renderer);
    CleanupOpenXR(xr);
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(vkInstance, nullptr);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();
    return 0;
}
