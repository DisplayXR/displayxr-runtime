// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS Vulkan OpenXR spinning cube with external window binding
 *
 * Demonstrates XR_EXT_macos_window_binding: the app creates its own
 * NSWindow + MetalView (with CAMetalLayer) and passes the NSView to
 * the runtime via the extension. The runtime renders into the app's
 * window instead of creating its own.
 *
 * Features:
 * - App creates and owns the NSWindow (XR_EXT_macos_window_binding)
 * - Mouse drag camera, WASD/QE movement, scroll zoom
 * - XR_EXT_display_info: Kooima projection, display metrics
 * - 2D/3D toggle (V key) via xrRequestDisplayModeEXT
 * - 1/2/3 keys for sim_display modes (SBS/anaglyph/blend)
 * - Tab: toggle HUD overlay, Space: reset camera, ESC: quit
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_macos_window_binding.h>
#include <openxr/XR_EXT_display_info.h>

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>
#include <vector>

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <unistd.h>

#include "stereo_params.h"
#include "display3d_view.h"

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
// Input state (ported from test_apps/common/input_handler.h)
// ============================================================================

struct InputState {
    // Mouse drag for camera look
    float yaw = 0.0f;
    float pitch = 0.0f;

    // WASD/QE movement keys (held state)
    bool keyW = false, keyA = false, keyS = false, keyD = false;
    bool keyE = false, keyQ = false;

    // Camera position
    float cameraPosX = 0.0f, cameraPosY = 0.0f, cameraPosZ = 0.0f;

    // View reset
    bool resetViewRequested = false;

    // Stereo camera parameters (IPD, parallax, perspective, scale/zoom)
    StereoParams stereo;

    // HUD toggle (Tab)
    bool hudVisible = true;

    // Display mode toggle (V key)
    bool displayMode3D = true;
    bool displayModeToggleRequested = false;
};

// ============================================================================
// Globals
// ============================================================================

static volatile bool g_running = true;
static NSWindow *g_window = nil;
static NSView *g_metalView = nil;
static InputState g_input;

// dlsym handle for sim_display live mode switching (Bug 4 fix)
typedef void (*PFN_sim_display_set_output_mode)(int mode);
static PFN_sim_display_set_output_mode g_pfnSetOutputMode = nullptr;

// Current sim_display output mode (0=SBS, 1=anaglyph, 2=blend)
// Tracks which mode the display processor is using so the app can
// adjust render dimensions and Kooima FOV accordingly.
// SBS: each eye = half width, Kooima uses half display width
// Anaglyph/blend: each eye = full width, Kooima uses full display width
static int g_currentOutputMode = 0;

// Borderless fullscreen state
static bool g_fullscreen = false;
static NSRect g_savedWindowFrame = {};
static NSUInteger g_savedWindowStyle = 0;

static void ToggleBorderlessFullscreen() {
    if (g_fullscreen) {
        // Restore windowed mode
        [g_window setStyleMask:g_savedWindowStyle];
        [g_window setFrame:g_savedWindowFrame display:YES animate:NO];
        [g_window setLevel:NSNormalWindowLevel];
        g_fullscreen = false;
        LOG_INFO("Exited fullscreen");
    } else {
        // Save state and go borderless fullscreen
        g_savedWindowStyle = [g_window styleMask];
        g_savedWindowFrame = [g_window frame];
        NSScreen *screen = [g_window screen] ?: [NSScreen mainScreen];
        [g_window setStyleMask:NSWindowStyleMaskBorderless];
        [g_window setFrame:[screen frame] display:YES animate:NO];
        [g_window setLevel:NSStatusWindowLevel]; // above menu bar
        g_fullscreen = true;
        LOG_INFO("Entered fullscreen");
    }
}

// Performance stats
static double g_avgFrameTime = 0.0;
static uint64_t g_frameCount = 0;
static float g_hudUpdateTimer = 0.0f;

// Render/window dimensions for HUD
static uint32_t g_renderW = 0, g_renderH = 0;
static uint32_t g_windowW = 0, g_windowH = 0;

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
    m[12] = tx;
    m[13] = ty;
    m[14] = tz;
}

static void mat4_scale(float* m, float sx, float sy, float sz) {
    mat4_identity(m);
    m[0] = sx;
    m[5] = sy;
    m[10] = sz;
}

static void mat4_rotation_y(float* m, float angle) {
    mat4_identity(m);
    float c = cosf(angle), s = sinf(angle);
    m[0] = c;   m[8] = s;
    m[2] = -s;  m[10] = c;
}

static void mat4_from_xr_fov(float* m, XrFovf fov, float nearZ, float farZ) {
    float tanL = tanf(fov.angleLeft);
    float tanR = tanf(fov.angleRight);
    float tanU = tanf(fov.angleUp);
    float tanD = tanf(fov.angleDown);
    float w = tanR - tanL;
    float h = tanU - tanD;
    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f / w;
    m[5]  = 2.0f / h;
    m[8]  = (tanR + tanL) / w;
    m[9]  = (tanU + tanD) / h;
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

static void mat4_view_from_xr_pose(float* viewMat, XrPosef pose) {
    float qx = pose.orientation.x, qy = pose.orientation.y;
    float qz = pose.orientation.z, qw = pose.orientation.w;

    float rot[16];
    mat4_identity(rot);
    rot[0]  = 1 - 2*(qy*qy + qz*qz);
    rot[1]  = 2*(qx*qy + qz*qw);
    rot[2]  = 2*(qx*qz - qy*qw);
    rot[4]  = 2*(qx*qy - qz*qw);
    rot[5]  = 1 - 2*(qx*qx + qz*qz);
    rot[6]  = 2*(qy*qz + qx*qw);
    rot[8]  = 2*(qx*qz + qy*qw);
    rot[9]  = 2*(qy*qz - qx*qw);
    rot[10] = 1 - 2*(qx*qx + qy*qy);

    // Transpose rotation (inverse of orthonormal)
    float invRot[16];
    mat4_identity(invRot);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            invRot[j*4+i] = rot[i*4+j];

    float invTrans[16];
    mat4_translation(invTrans, -pose.position.x, -pose.position.y, -pose.position.z);
    mat4_multiply(viewMat, invRot, invTrans);
}

// Quaternion from yaw/pitch (matches DirectX::XMQuaternionRotationRollPitchYaw(pitch, yaw, 0))
static void quat_from_yaw_pitch(float yaw, float pitch, XrQuaternionf* out) {
    float cy = cosf(yaw / 2.0f), sy = sinf(yaw / 2.0f);
    float cp = cosf(pitch / 2.0f), sp = sinf(pitch / 2.0f);
    out->w = cy * cp;
    out->x = cy * sp;
    out->y = sy * cp;
    out->z = -sy * sp;
}

// Rotate vec3 by quaternion: v' = q * v * q^-1
static void quat_rotate_vec3(XrQuaternionf q, float vx, float vy, float vz,
    float* ox, float* oy, float* oz) {
    float tx = 2.0f * (q.y * vz - q.z * vy);
    float ty = 2.0f * (q.z * vx - q.x * vz);
    float tz = 2.0f * (q.x * vy - q.y * vx);
    *ox = vx + q.w * tx + (q.y * tz - q.z * ty);
    *oy = vy + q.w * ty + (q.z * tx - q.x * tz);
    *oz = vz + q.w * tz + (q.x * ty - q.y * tx);
}

// Hamilton product: a * b
static XrQuaternionf quat_multiply(XrQuaternionf a, XrQuaternionf b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
    };
}

// Kooima projection and FOV now provided by display3d_view.h

// ============================================================================
// SPIR-V shaders (embedded)
// ============================================================================
// Cube vertex shader: push constant MVP, per-vertex color
static const uint32_t cubeVertSpv[] = {
    0x07230203,0x00010000,0x000d000a,0x00000028,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0009000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x0000000d,0x00000012,0x0000001c,
    0x00000025,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,
    0x00060005,0x0000000b,0x505f6c67,0x65567265,0x78657472,0x00000000,0x00060006,0x0000000b,
    0x00000000,0x505f6c67,0x7469736f,0x006e6f69,0x00030005,0x0000000d,0x00000000,0x00050005,
    0x0000000f,0x68737550,0x736e6f43,0x00000074,0x00040006,0x0000000f,0x00000000,0x0070766d,
    0x00030005,0x00000011,0x00006370,0x00040005,0x00000012,0x6f506e69,0x00000073,0x00050005,
    0x0000001c,0x4374756f,0x726f6c6f,0x00000000,0x00040005,0x00000025,0x6f436e69,0x00726f6c,
    0x00050048,0x0000000b,0x00000000,0x0000000b,0x00000000,0x00030047,0x0000000b,0x00000002,
    0x00040048,0x0000000f,0x00000000,0x00000005,0x00050048,0x0000000f,0x00000000,0x00000023,
    0x00000000,0x00050048,0x0000000f,0x00000000,0x00000007,0x00000010,0x00030047,0x0000000f,
    0x00000002,0x00040047,0x00000012,0x0000001e,0x00000000,0x00040047,0x0000001c,0x0000001e,
    0x00000000,0x00040047,0x00000025,0x0000001e,0x00000001,0x00020013,0x00000002,0x00030021,
    0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,
    0x00000004,0x0003001e,0x0000000b,0x00000007,0x00040020,0x0000000c,0x00000003,0x0000000b,
    0x0004003b,0x0000000c,0x0000000d,0x00000003,0x00040015,0x0000000e,0x00000020,0x00000001,
    0x00040018,0x00000010,0x00000007,0x00000004,0x0003001e,0x0000000f,0x00000010,0x00040020,
    0x00000014,0x00000009,0x0000000f,0x0004003b,0x00000014,0x00000011,0x00000009,0x0004002b,
    0x0000000e,0x00000013,0x00000000,0x00040020,0x00000015,0x00000009,0x00000010,0x00040017,
    0x00000018,0x00000006,0x00000003,0x00040020,0x00000019,0x00000001,0x00000018,0x0004003b,
    0x00000019,0x00000012,0x00000001,0x0004002b,0x00000006,0x0000001a,0x3f800000,0x00040020,
    0x0000001b,0x00000003,0x00000007,0x0004003b,0x0000001b,0x0000001c,0x00000003,0x00040020,
    0x00000024,0x00000001,0x00000007,0x0004003b,0x00000024,0x00000025,0x00000001,0x00050036,
    0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x00050041,0x00000015,
    0x00000016,0x00000011,0x00000013,0x0004003d,0x00000010,0x00000017,0x00000016,0x0004003d,
    0x00000018,0x0000001d,0x00000012,0x00050051,0x00000006,0x0000001e,0x0000001d,0x00000000,
    0x00050051,0x00000006,0x0000001f,0x0000001d,0x00000001,0x00050051,0x00000006,0x00000020,
    0x0000001d,0x00000002,0x00070050,0x00000007,0x00000021,0x0000001e,0x0000001f,0x00000020,
    0x0000001a,0x00050091,0x00000007,0x00000022,0x00000017,0x00000021,0x00050041,0x0000001b,
    0x00000023,0x0000000d,0x00000013,0x0003003e,0x00000023,0x00000022,0x0004003d,0x00000007,
    0x00000026,0x00000025,0x0003003e,0x0000001c,0x00000026,0x000100fd,0x00010038,
};

// Cube fragment shader: pass-through color
static const uint32_t cubeFragSpv[] = {
    0x07230203,0x00010000,0x000d000a,0x0000000d,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0007000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x00000009,0x0000000b,0x00030010,
    0x00000004,0x00000007,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,
    0x00000000,0x00050005,0x00000009,0x4374756f,0x726f6c6f,0x00000000,0x00040005,0x0000000b,
    0x6f436e69,0x00726f6c,0x00040047,0x00000009,0x0000001e,0x00000000,0x00040047,0x0000000b,
    0x0000001e,0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,
    0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,0x00040020,0x00000008,
    0x00000003,0x00000007,0x0004003b,0x00000008,0x00000009,0x00000003,0x00040020,0x0000000a,
    0x00000001,0x00000007,0x0004003b,0x0000000a,0x0000000b,0x00000001,0x00050036,0x00000002,
    0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003d,0x00000007,0x0000000c,
    0x0000000b,0x0003003e,0x00000009,0x0000000c,0x000100fd,0x00010038,
};

// Grid vertex shader: push constant MVP, fixed gray color
static const uint32_t gridVertSpv[] = {
    0x07230203,0x00010000,0x000d000a,0x00000024,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0008000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x0000000d,0x00000012,0x0000001c,
    0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00060005,
    0x0000000b,0x505f6c67,0x65567265,0x78657472,0x00000000,0x00060006,0x0000000b,0x00000000,
    0x505f6c67,0x7469736f,0x006e6f69,0x00030005,0x0000000d,0x00000000,0x00050005,0x0000000f,
    0x68737550,0x736e6f43,0x00000074,0x00040006,0x0000000f,0x00000000,0x0070766d,0x00030005,
    0x00000011,0x00006370,0x00040005,0x00000012,0x6f506e69,0x00000073,0x00050005,0x0000001c,
    0x4374756f,0x726f6c6f,0x00000000,0x00050048,0x0000000b,0x00000000,0x0000000b,0x00000000,
    0x00030047,0x0000000b,0x00000002,0x00040048,0x0000000f,0x00000000,0x00000005,0x00050048,
    0x0000000f,0x00000000,0x00000023,0x00000000,0x00050048,0x0000000f,0x00000000,0x00000007,
    0x00000010,0x00030047,0x0000000f,0x00000002,0x00040047,0x00000012,0x0000001e,0x00000000,
    0x00040047,0x0000001c,0x0000001e,0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,
    0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,
    0x0003001e,0x0000000b,0x00000007,0x00040020,0x0000000c,0x00000003,0x0000000b,0x0004003b,
    0x0000000c,0x0000000d,0x00000003,0x00040015,0x0000000e,0x00000020,0x00000001,0x00040018,
    0x00000010,0x00000007,0x00000004,0x0003001e,0x0000000f,0x00000010,0x00040020,0x00000014,
    0x00000009,0x0000000f,0x0004003b,0x00000014,0x00000011,0x00000009,0x0004002b,0x0000000e,
    0x00000013,0x00000000,0x00040020,0x00000015,0x00000009,0x00000010,0x00040017,0x00000018,
    0x00000006,0x00000003,0x00040020,0x00000019,0x00000001,0x00000018,0x0004003b,0x00000019,
    0x00000012,0x00000001,0x0004002b,0x00000006,0x0000001a,0x3f800000,0x00040020,0x0000001b,
    0x00000003,0x00000007,0x0004003b,0x0000001b,0x0000001c,0x00000003,0x0004002b,0x00000006,
    0x0000001d,0x3e4ccccd,0x0004002b,0x00000006,0x0000001e,0x3e99999a,0x00050036,0x00000002,
    0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x00050041,0x00000015,0x00000016,
    0x00000011,0x00000013,0x0004003d,0x00000010,0x00000017,0x00000016,0x0004003d,0x00000018,
    0x00000020,0x00000012,0x00050051,0x00000006,0x00000021,0x00000020,0x00000000,0x00050051,
    0x00000006,0x00000022,0x00000020,0x00000001,0x00050051,0x00000006,0x00000023,0x00000020,
    0x00000002,0x00070050,0x00000007,0x0000001f,0x00000021,0x00000022,0x00000023,0x0000001a,
    0x00050091,0x00000007,0x00000009,0x00000017,0x0000001f,0x00050041,0x0000001b,0x0000000a,
    0x0000000d,0x00000013,0x0003003e,0x0000000a,0x00000009,0x00070050,0x00000007,0x00000008,
    0x0000001d,0x0000001e,0x0000001d,0x0000001a,0x0003003e,0x0000001c,0x00000008,0x000100fd,
    0x00010038,
};

// Grid fragment shader: pass-through color
static const uint32_t gridFragSpv[] = {
    0x07230203,0x00010000,0x000d000a,0x0000000d,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0007000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x00000009,0x0000000b,0x00030010,
    0x00000004,0x00000007,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,
    0x00000000,0x00050005,0x00000009,0x4374756f,0x726f6c6f,0x00000000,0x00040005,0x0000000b,
    0x6f436e69,0x00726f6c,0x00040047,0x00000009,0x0000001e,0x00000000,0x00040047,0x0000000b,
    0x0000001e,0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,
    0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,0x00040020,0x00000008,
    0x00000003,0x00000007,0x0004003b,0x00000008,0x00000009,0x00000003,0x00040020,0x0000000a,
    0x00000001,0x00000007,0x0004003b,0x0000000a,0x0000000b,0x00000001,0x00050036,0x00000002,
    0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003d,0x00000007,0x0000000c,
    0x0000000b,0x0003003e,0x00000009,0x0000000c,0x000100fd,0x00010038,
};

// Textured cube vertex shader: push constants (MVP + model), outputs UV/normal/tangent
static const uint32_t cubeTexturedVertSpv[] = {
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
static const uint32_t cubeTexturedFragSpv[] = {
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

// ============================================================================
// Vulkan renderer structures (same as sim_cube_openxr)
// ============================================================================

struct CubeVertex { float pos[3]; float color[4]; float uv[2]; float normal[3]; float tangent[3]; };
struct GridVertex { float pos[3]; };

struct SwapchainFramebuffers {
    std::vector<VkImageView> colorViews;
    std::vector<VkImageView> depthViews;
    std::vector<VkFramebuffer> framebuffers;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct VkRenderer {
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;       // grid: push constants only (64 bytes)
    VkPipelineLayout cubePipelineLayout = VK_NULL_HANDLE;   // textured cube: descriptor set + push constants (128 bytes)
    VkPipeline cubePipeline = VK_NULL_HANDLE;
    VkPipeline gridPipeline = VK_NULL_HANDLE;
    VkBuffer cubeVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cubeVertexMemory = VK_NULL_HANDLE;
    VkBuffer cubeIndexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cubeIndexMemory = VK_NULL_HANDLE;
    VkBuffer gridVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory gridVertexMemory = VK_NULL_HANDLE;
    int gridVertexCount = 0;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence frameFence = VK_NULL_HANDLE;
    float cubeRotation = 0.0f;
    SwapchainFramebuffers swapchainFBs;
    VkRenderPass renderPassLoad = VK_NULL_HANDLE;
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
// Texture path helper
// ============================================================================

static std::string GetTextureDir() {
    char pathBuf[4096];
    uint32_t pathSize = sizeof(pathBuf);
    if (_NSGetExecutablePath(pathBuf, &pathSize) == 0) {
        // Find last '/' to get directory
        char* lastSlash = strrchr(pathBuf, '/');
        if (lastSlash) *lastSlash = '\0';
        return std::string(pathBuf) + "/textures/";
    }
    return "textures/";
}

// ============================================================================
// Vulkan helpers (same as sim_cube_openxr)
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

static bool CreateBuffer(VkDevice device, VkPhysicalDevice physDevice, VkDeviceSize size,
    VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
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
    allocInfo.memoryTypeIndex = FindMemoryType(physDevice, memReqs.memoryTypeBits, properties);
    if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(device, buffer, memory, 0);
    return true;
}

static bool CreateDepthImage(VkDevice device, VkPhysicalDevice physDevice,
    uint32_t width, uint32_t height,
    VkImage& image, VkDeviceMemory& memory)
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
    if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, image, &memReqs);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(physDevice, memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) return false;
    vkBindImageMemory(device, image, memory, 0);
    return true;
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

    // Copy staging → image via one-shot command buffer
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

    // Transition UNDEFINED → TRANSFER_DST
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

    // Transition TRANSFER_DST → SHADER_READ_ONLY
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

// Store physDevice globally for depth image creation
static VkPhysicalDevice g_physDevice = VK_NULL_HANDLE;

static bool CreateSwapchainFramebuffers(VkRenderer& renderer,
    VkImage* images, uint32_t imageCount,
    uint32_t width, uint32_t height, VkFormat colorFormat)
{
    auto& fb = renderer.swapchainFBs;
    fb.width = width;
    fb.height = height;
    fb.colorViews.resize(imageCount);
    fb.depthViews.resize(imageCount);
    fb.framebuffers.resize(imageCount);

    // g_physDevice is set during GetVulkanPhysicalDevice
    VkPhysicalDevice physDevice = g_physDevice;

    if (!CreateDepthImage(renderer.device, physDevice, width, height,
        fb.depthImage, fb.depthMemory)) {
        return false;
    }

    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = colorFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(renderer.device, &viewInfo, nullptr, &fb.colorViews[i]) != VK_SUCCESS)
            return false;

        VkImageViewCreateInfo depthViewInfo = {};
        depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        depthViewInfo.image = fb.depthImage;
        depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        depthViewInfo.format = VK_FORMAT_D32_SFLOAT;
        depthViewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(renderer.device, &depthViewInfo, nullptr, &fb.depthViews[i]) != VK_SUCCESS)
            return false;

        VkImageView attachments[] = {fb.colorViews[i], fb.depthViews[i]};
        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = renderer.renderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = width;
        fbInfo.height = height;
        fbInfo.layers = 1;
        if (vkCreateFramebuffer(renderer.device, &fbInfo, nullptr, &fb.framebuffers[i]) != VK_SUCCESS)
            return false;
    }
    return true;
}

static bool InitializeVkRenderer(VkRenderer& renderer, VkDevice device, VkPhysicalDevice physDevice,
    VkQueue graphicsQueue, uint32_t queueFamilyIndex, VkFormat colorFormat)
{
    renderer.device = device;
    renderer.graphicsQueue = graphicsQueue;
    g_physDevice = physDevice;

    // Render pass
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = colorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

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
    VK_CHECK(vkCreateRenderPass(device, &rpInfo, nullptr, &renderer.renderPass));

    // Create second render pass with LOAD instead of CLEAR (for second eye in SBS)
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VK_CHECK(vkCreateRenderPass(device, &rpInfo, nullptr, &renderer.renderPassLoad));

    // Grid pipeline layout (push constants only, 64 bytes)
    VkPushConstantRange gridPushRange = {};
    gridPushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    gridPushRange.offset = 0;
    gridPushRange.size = 64; // MVP matrix

    VkPipelineLayoutCreateInfo gridLayoutInfo = {};
    gridLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    gridLayoutInfo.pushConstantRangeCount = 1;
    gridLayoutInfo.pPushConstantRanges = &gridPushRange;
    VK_CHECK(vkCreatePipelineLayout(device, &gridLayoutInfo, nullptr, &renderer.pipelineLayout));

    // Descriptor set layout for textured cube (3 combined image samplers)
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
    VK_CHECK(vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &renderer.descriptorSetLayout));

    // Cube pipeline layout (descriptor set + 128 bytes push constants: MVP + model)
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
    VK_CHECK(vkCreatePipelineLayout(device, &cubeLayoutInfo, nullptr, &renderer.cubePipelineLayout));

    // Shader modules
    auto createShaderModule = [&](const uint32_t* code, size_t size) -> VkShaderModule {
        VkShaderModuleCreateInfo ci = {};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = size;
        ci.pCode = code;
        VkShaderModule sm;
        vkCreateShaderModule(device, &ci, nullptr, &sm);
        return sm;
    };

    VkShaderModule cubeVert = createShaderModule(cubeTexturedVertSpv, sizeof(cubeTexturedVertSpv));
    VkShaderModule cubeFrag = createShaderModule(cubeTexturedFragSpv, sizeof(cubeTexturedFragSpv));
    VkShaderModule gridVert = createShaderModule(gridVertSpv, sizeof(gridVertSpv));
    VkShaderModule gridFrag = createShaderModule(gridFragSpv, sizeof(gridFragSpv));

    // Common pipeline states
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

    // Textured cube pipeline
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
        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &renderer.cubePipeline));
    }

    // Grid pipeline
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
        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &renderer.gridPipeline));
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
        0,1,2, 0,2,3, 4,5,6, 4,6,7, 8,9,10, 8,10,11,
        12,13,14, 12,14,15, 16,17,18, 16,18,19, 20,21,22, 20,22,23,
    };

    if (!CreateBuffer(device, physDevice, sizeof(cubeVerts),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.cubeVertexBuffer, renderer.cubeVertexMemory)) return false;
    void* data;
    vkMapMemory(device, renderer.cubeVertexMemory, 0, sizeof(cubeVerts), 0, &data);
    memcpy(data, cubeVerts, sizeof(cubeVerts));
    vkUnmapMemory(device, renderer.cubeVertexMemory);

    if (!CreateBuffer(device, physDevice, sizeof(cubeIndices),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.cubeIndexBuffer, renderer.cubeIndexMemory)) return false;
    vkMapMemory(device, renderer.cubeIndexMemory, 0, sizeof(cubeIndices), 0, &data);
    memcpy(data, cubeIndices, sizeof(cubeIndices));
    vkUnmapMemory(device, renderer.cubeIndexMemory);

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
        renderer.gridVertexBuffer, renderer.gridVertexMemory)) return false;
    vkMapMemory(device, renderer.gridVertexMemory, 0, gridBufSize, 0, &data);
    memcpy(data, gridVerts.data(), (size_t)gridBufSize);
    vkUnmapMemory(device, renderer.gridVertexMemory);

    // Command pool + buffer
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndex;
    VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &renderer.commandPool));

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = renderer.commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &renderer.commandBuffer));

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &renderer.frameFence));

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
        VK_CHECK(vkCreateSampler(device, &samplerInfo, nullptr, &renderer.texSampler));

        // Descriptor pool and set
        VkDescriptorPoolSize poolSize = {};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 3;
        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &renderer.descriptorPool));

        VkDescriptorSetAllocateInfo dsAllocInfo = {};
        dsAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsAllocInfo.descriptorPool = renderer.descriptorPool;
        dsAllocInfo.descriptorSetCount = 1;
        dsAllocInfo.pSetLayouts = &renderer.descriptorSetLayout;
        VK_CHECK(vkAllocateDescriptorSets(device, &dsAllocInfo, &renderer.descriptorSet));

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

struct EyeRenderParams {
    uint32_t viewportX, viewportY, width, height;
    float viewMat[16];
    float projMat[16];
};

static void RenderScene(VkRenderer& renderer, uint32_t imageIndex,
    const EyeRenderParams* eyes, int eyeCount)
{
    VkDevice device = renderer.device;
    vkWaitForFences(device, 1, &renderer.frameFence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &renderer.frameFence);

    auto& fb = renderer.swapchainFBs;
    VkCommandBuffer cmd = renderer.commandBuffer;
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkClearValue clearValues[2] = {};
    clearValues[0].color = {{0.05f, 0.05f, 0.1f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin = {};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = renderer.renderPass;
    rpBegin.framebuffer = fb.framebuffers[imageIndex];
    rpBegin.renderArea = {{0, 0}, {fb.width, fb.height}};
    rpBegin.clearValueCount = 2;
    rpBegin.pClearValues = clearValues;
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // Render all eyes in a single render pass — just change viewport/scissor
    for (int eye = 0; eye < eyeCount; eye++) {
        const auto& e = eyes[eye];

        VkViewport viewport = {(float)e.viewportX, (float)(e.viewportY + e.height),
            (float)e.width, -(float)e.height, 0.0f, 1.0f};
        VkRect2D scissor = {{(int32_t)e.viewportX, (int32_t)e.viewportY}, {e.width, e.height}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        float vp[16];
        mat4_multiply(vp, e.projMat, e.viewMat);

        // Draw grid
        {
            const float gridScale = 0.05f;
            float gridWorld[16], gridScl[16], gridTrans[16];
            mat4_scale(gridScl, gridScale, gridScale, gridScale);
            mat4_translation(gridTrans, 0, gridScale, 0);
            mat4_multiply(gridWorld, gridTrans, gridScl);
            float gridMvp[16];
            mat4_multiply(gridMvp, vp, gridWorld);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.gridPipeline);
            vkCmdPushConstants(cmd, renderer.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, gridMvp);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &renderer.gridVertexBuffer, &offset);
            vkCmdDraw(cmd, renderer.gridVertexCount, 1, 0, 0);
        }

        // Draw cube (textured)
        {
            const float cubeSize = 0.06f;
            const float cubeHeight = cubeSize / 2.0f;
            float rot[16], scl[16], trans[16], model[16], tmp[16];
            mat4_rotation_y(rot, renderer.cubeRotation);
            mat4_scale(scl, cubeSize, cubeSize, cubeSize);
            mat4_translation(trans, 0, cubeHeight, 0);
            mat4_multiply(tmp, scl, rot);
            mat4_multiply(model, trans, tmp);
            float mvp[16];
            mat4_multiply(mvp, vp, model);

            // Push constants: MVP (64 bytes) + model (64 bytes) = 128 bytes
            float pushData[32];
            memcpy(pushData, mvp, 64);
            memcpy(pushData + 16, model, 64);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.cubePipeline);
            vkCmdPushConstants(cmd, renderer.cubePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 128, pushData);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.cubePipelineLayout,
                0, 1, &renderer.descriptorSet, 0, nullptr);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &renderer.cubeVertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, renderer.cubeIndexBuffer, 0, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(cmd, 36, 1, 0, 0, 0);
        }
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(renderer.graphicsQueue, 1, &submitInfo, renderer.frameFence);
}

static void CleanupVkRenderer(VkRenderer& renderer) {
    if (renderer.device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(renderer.device);

    {
        auto& fb = renderer.swapchainFBs;
        for (auto f : fb.framebuffers) vkDestroyFramebuffer(renderer.device, f, nullptr);
        for (auto v : fb.colorViews) vkDestroyImageView(renderer.device, v, nullptr);
        for (auto v : fb.depthViews) vkDestroyImageView(renderer.device, v, nullptr);
        if (fb.depthImage) vkDestroyImage(renderer.device, fb.depthImage, nullptr);
        if (fb.depthMemory) vkFreeMemory(renderer.device, fb.depthMemory, nullptr);
    }

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
    // Texture cleanup
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
    if (renderer.renderPassLoad) vkDestroyRenderPass(renderer.device, renderer.renderPassLoad, nullptr);
    if (renderer.renderPass) vkDestroyRenderPass(renderer.device, renderer.renderPass, nullptr);
}

// ============================================================================
// HUD overlay view (macOS native text overlay)
// ============================================================================

@interface HudOverlayView : NSView
@property (nonatomic, copy) NSString *hudText;
@end

@implementation HudOverlayView
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) { _hudText = @""; [self setWantsLayer:YES]; }
    return self;
}
- (BOOL)isOpaque { return NO; }
- (NSView *)hitTest:(NSPoint)point { (void)point; return nil; } // pass clicks through
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    if (_hudText.length == 0) return;
    NSBezierPath *bg = [NSBezierPath bezierPathWithRoundedRect:self.bounds xRadius:6 yRadius:6];
    [[NSColor colorWithCalibratedRed:0 green:0 blue:0 alpha:0.5] setFill];
    [bg fill];
    NSFont *font = [NSFont fontWithName:@"Menlo" size:11];
    if (!font) font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    NSDictionary *attrs = @{
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: [NSColor colorWithCalibratedRed:0.9 green:0.9 blue:0.9 alpha:1.0]
    };
    NSRect textRect = NSInsetRect(self.bounds, 8, 8);
    [_hudText drawWithRect:textRect
                   options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine
                attributes:attrs context:nil];
}
@end

static HudOverlayView *g_hudView = nil;

// ============================================================================
// macOS Window + Metal View
// ============================================================================

/*!
 * NSView subclass whose backing layer is a CAMetalLayer.
 * Same pattern as MonadoMetalView in comp_window_macos.m.
 */
@interface AppMetalView : NSView
@end

@implementation AppMetalView
- (CALayer *)makeBackingLayer {
    return [CAMetalLayer layer];
}
- (BOOL)wantsUpdateLayer {
    return YES;
}
@end

// NSApplicationDelegate: prevent termination when the window is closed or app
// loses focus.  Without this, macOS terminates CLI-launched GUI apps as soon as
// the last window closes (applicationShouldTerminateAfterLastWindowClosed
// defaults to YES).
@interface AppDelegate : NSObject <NSApplicationDelegate>
@end
@implementation AppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return NO;
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    (void)sender;
    g_running = false;
    return NSTerminateCancel; // let our loop handle cleanup
}
@end

// NSWindowDelegate: translate window-close into g_running = false so the render
// loop exits gracefully; also keeps the app alive on focus loss.
@interface AppWindowDelegate : NSObject <NSWindowDelegate>
@end
@implementation AppWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;
    g_running = false;
    return NO; // we handle teardown ourselves
}
@end

static AppDelegate *g_appDelegate = nil;
static AppWindowDelegate *g_windowDelegate = nil;

static bool CreateMacOSWindow(uint32_t width, uint32_t height) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        g_appDelegate = [[AppDelegate alloc] init];
        [NSApp setDelegate:g_appDelegate];

        NSRect frame = NSMakeRect(100, 100, width, height);
        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable;

        g_window = [[NSWindow alloc] initWithContentRect:frame
                                               styleMask:style
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];

        [g_window setTitle:@"Sim Cube OpenXR (External Window)"];
        [g_window setAcceptsMouseMovedEvents:YES];
        [g_window setReleasedWhenClosed:NO];

        g_windowDelegate = [[AppWindowDelegate alloc] init];
        [g_window setDelegate:g_windowDelegate];

        g_metalView = [[AppMetalView alloc] initWithFrame:frame];
        [g_metalView setWantsLayer:YES];

        [g_window setContentView:g_metalView];

        // HUD overlay (semi-transparent text overlay, positioned bottom-left)
        NSRect hudFrame = NSMakeRect(10, 10, 420, 305);
        g_hudView = [[HudOverlayView alloc] initWithFrame:hudFrame];
        [g_metalView addSubview:g_hudView];

        [g_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        // Pump events so the window appears
        NSEvent *event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                           untilDate:nil
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES]) != nil) {
            [NSApp sendEvent:event];
        }
    }

    if (g_window == nil || g_metalView == nil) {
        LOG_ERROR("Failed to create macOS window");
        return false;
    }

    LOG_INFO("Created macOS window %ux%u with CAMetalLayer-backed NSView", width, height);
    return true;
}

static void PumpMacOSEvents() {
    // Track whether left-click started in content area vs title bar.
    // Title bar drags should move the window, not rotate the scene.
    static bool leftDragInContent = false;

    @autoreleasepool {
        NSEvent *event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                           untilDate:nil
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES]) != nil) {
            NSEventType type = [event type];

            if (type == NSEventTypeLeftMouseDown) {
                NSPoint loc = [event locationInWindow];
                NSRect contentRect = g_window ? [[g_window contentView] frame] : NSZeroRect;
                leftDragInContent = NSMouseInRect(loc, contentRect, NO);
                if ([event clickCount] >= 2) g_input.resetViewRequested = true;
            } else if (type == NSEventTypeLeftMouseDragged) {
                // Rotate camera only while left button is physically held AND
                // the drag started in the content area (not the title bar).
                // Uses event deltas — no persistent drag state that can get
                // stuck when the window-resize modal loop swallows MouseUp.
                if (leftDragInContent && ([NSEvent pressedMouseButtons] & 1)) {
                    g_input.yaw   -= (float)[event deltaX] * 0.005f;
                    g_input.pitch -= (float)[event deltaY] * 0.005f;
                    if (g_input.pitch > 1.4f) g_input.pitch = 1.4f;
                    if (g_input.pitch < -1.4f) g_input.pitch = -1.4f;
                }
            } else if (type == NSEventTypeScrollWheel) {
                float dy = (float)[event scrollingDeltaY];
                float factor = (dy > 0) ? 1.1f : (1.0f / 1.1f);
                NSUInteger scrollMods = [event modifierFlags];
                if (scrollMods & NSEventModifierFlagShift) {
                    g_input.stereo.ipdFactor *= factor;
                    if (g_input.stereo.ipdFactor < 0.0f) g_input.stereo.ipdFactor = 0.0f;
                    if (g_input.stereo.ipdFactor > 1.0f) g_input.stereo.ipdFactor = 1.0f;
                } else if (scrollMods & NSEventModifierFlagControl) {
                    g_input.stereo.parallaxFactor *= factor;
                    if (g_input.stereo.parallaxFactor < 0.0f) g_input.stereo.parallaxFactor = 0.0f;
                    if (g_input.stereo.parallaxFactor > 1.0f) g_input.stereo.parallaxFactor = 1.0f;
                } else if (scrollMods & NSEventModifierFlagOption) {
                    g_input.stereo.perspectiveFactor *= factor;
                    if (g_input.stereo.perspectiveFactor < 0.1f) g_input.stereo.perspectiveFactor = 0.1f;
                    if (g_input.stereo.perspectiveFactor > 10.0f) g_input.stereo.perspectiveFactor = 10.0f;
                } else {
                    g_input.stereo.scaleFactor *= factor;
                    if (g_input.stereo.scaleFactor < 0.1f) g_input.stereo.scaleFactor = 0.1f;
                    if (g_input.stereo.scaleFactor > 10.0f) g_input.stereo.scaleFactor = 10.0f;
                }
            } else if (type == NSEventTypeKeyDown) {
                // Cmd+Ctrl+F fullscreen toggle (keyCode 3 = 'f', ignores character remapping)
                NSUInteger mods = [event modifierFlags];
                if ([event keyCode] == 3 &&
                    (mods & NSEventModifierFlagCommand) &&
                    (mods & NSEventModifierFlagControl) &&
                    ![event isARepeat]) {
                    ToggleBorderlessFullscreen();
                }
                else if ([[event characters] length] > 0) {
                    unichar ch = tolower([[event characters] characterAtIndex:0]);
                    bool isRepeat = [event isARepeat];
                    if (ch == 27) { g_running = false; }
                    else if (ch == 'w') { g_input.keyW = true; }
                    else if (ch == 'a') { g_input.keyA = true; }
                    else if (ch == 's') { g_input.keyS = true; }
                    else if (ch == 'd') { g_input.keyD = true; }
                    else if (ch == 'e') { g_input.keyE = true; }
                    else if (ch == 'q') { g_input.keyQ = true; }
                    else if (ch == ' ') { g_input.resetViewRequested = true; }
                    else if (ch == '\t' && !isRepeat) { g_input.hudVisible = !g_input.hudVisible; }
                    else if (ch == 'v' && !isRepeat) {
                        g_input.displayMode3D = !g_input.displayMode3D;
                        g_input.displayModeToggleRequested = true;
                    }
                    else if (ch == '1' && !isRepeat) {
                        g_currentOutputMode = 0;
                        if (g_pfnSetOutputMode) {
                            g_pfnSetOutputMode(0); // SIM_DISPLAY_OUTPUT_SBS
                            LOG_INFO("Switched to SBS mode (live)");
                        } else {
                            setenv("SIM_DISPLAY_OUTPUT", "sbs", 1);
                            LOG_INFO("Switched to SBS mode (restart required)");
                        }
                    }
                    else if (ch == '2' && !isRepeat) {
                        g_currentOutputMode = 1;
                        if (g_pfnSetOutputMode) {
                            g_pfnSetOutputMode(1); // SIM_DISPLAY_OUTPUT_ANAGLYPH
                            LOG_INFO("Switched to anaglyph mode (live)");
                        } else {
                            setenv("SIM_DISPLAY_OUTPUT", "anaglyph", 1);
                            LOG_INFO("Switched to anaglyph mode (restart required)");
                        }
                    }
                    else if (ch == '3' && !isRepeat) {
                        g_currentOutputMode = 2;
                        if (g_pfnSetOutputMode) {
                            g_pfnSetOutputMode(2); // SIM_DISPLAY_OUTPUT_BLEND
                            LOG_INFO("Switched to blend mode (live)");
                        } else {
                            setenv("SIM_DISPLAY_OUTPUT", "blend", 1);
                            LOG_INFO("Switched to blend mode (restart required)");
                        }
                    }
                }
            } else if (type == NSEventTypeKeyUp) {
                if ([[event characters] length] > 0) {
                    unichar ch = tolower([[event characters] characterAtIndex:0]);
                    if (ch == 'w') g_input.keyW = false;
                    else if (ch == 'a') g_input.keyA = false;
                    else if (ch == 's') g_input.keyS = false;
                    else if (ch == 'd') g_input.keyD = false;
                    else if (ch == 'e') g_input.keyE = false;
                    else if (ch == 'q') g_input.keyQ = false;
                }
            }

            // Forward non-key events to NSApp; skip key events to prevent beep
            if (type != NSEventTypeKeyDown && type != NSEventTypeKeyUp) {
                [NSApp sendEvent:event];
            }
        }

        // Update window dimensions from actual drawable size (handles resize + fullscreen).
        // Multiply by backingScaleFactor to get pixel dimensions matching the Vulkan
        // surface extent (CAMetalLayer uses pixel dimensions on Retina).
        if (g_window != nil) {
            NSSize contentSize = [[g_window contentView] bounds].size;
            CGFloat backingScale = [g_window backingScaleFactor];
            g_windowW = (uint32_t)(contentSize.width * backingScale);
            g_windowH = (uint32_t)(contentSize.height * backingScale);
        }
    }
}

// ============================================================================
// Camera movement (ported from test_apps/common/input_handler.cpp)
// ============================================================================

static void UpdateCameraMovement(InputState& state, float deltaTime) {
    if (state.resetViewRequested) {
        state.cameraPosX = state.cameraPosY = state.cameraPosZ = 0.0f;
        state.yaw = state.pitch = 0.0f;
        state.stereo = StereoParams{};
        state.resetViewRequested = false;
        return;
    }

    const float moveSpeed = 0.1f / state.stereo.scaleFactor;
    XrQuaternionf ori;
    quat_from_yaw_pitch(state.yaw, state.pitch, &ori);

    float fwdX, fwdY, fwdZ, rtX, rtY, rtZ, upX, upY, upZ;
    quat_rotate_vec3(ori, 0, 0, -1, &fwdX, &fwdY, &fwdZ);
    quat_rotate_vec3(ori, 1, 0, 0, &rtX, &rtY, &rtZ);
    quat_rotate_vec3(ori, 0, 1, 0, &upX, &upY, &upZ);

    float d = moveSpeed * deltaTime;
    if (state.keyW) { state.cameraPosX += fwdX*d; state.cameraPosY += fwdY*d; state.cameraPosZ += fwdZ*d; }
    if (state.keyS) { state.cameraPosX -= fwdX*d; state.cameraPosY -= fwdY*d; state.cameraPosZ -= fwdZ*d; }
    if (state.keyD) { state.cameraPosX += rtX*d; state.cameraPosY += rtY*d; state.cameraPosZ += rtZ*d; }
    if (state.keyA) { state.cameraPosX -= rtX*d; state.cameraPosY -= rtY*d; state.cameraPosZ -= rtZ*d; }
    if (state.keyE) { state.cameraPosX += upX*d; state.cameraPosY += upY*d; state.cameraPosZ += upZ*d; }
    if (state.keyQ) { state.cameraPosX -= upX*d; state.cameraPosY -= upY*d; state.cameraPosZ -= upZ*d; }
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
    XrSpace displaySpace = XR_NULL_HANDLE;

    SwapchainInfo swapchain;

    uint32_t displayPixelWidth = 0;
    uint32_t displayPixelHeight = 0;

    XrViewConfigurationType viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    std::vector<XrViewConfigurationView> configViews;

    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;
    bool exitRequested = false;
    bool hasMacosWindowBinding = false;

    // XR_EXT_display_info
    bool hasDisplayInfoExt = false;
    float recommendedViewScaleX = 1.0f;
    float recommendedViewScaleY = 1.0f;
    float displayWidthM = 0.0f;
    float displayHeightM = 0.0f;
    float nominalViewerX = 0.0f;
    float nominalViewerY = 0.0f;
    float nominalViewerZ = 0.5f;
    bool supportsDisplayModeSwitch = false;
    PFN_xrRequestDisplayModeEXT pfnRequestDisplayModeEXT = nullptr;

    // Eye tracking data for HUD
    float leftEyeX = 0, leftEyeY = 0, leftEyeZ = 0;
    float rightEyeX = 0, rightEyeY = 0, rightEyeZ = 0;
    bool eyeTrackingActive = false;

    // System name from xrGetSystemProperties
    char systemName[XR_MAX_SYSTEM_NAME_SIZE] = {};
};

static bool InitializeOpenXR(AppXrSession& xr) {
    LOG_INFO("Initializing OpenXR...");

    uint32_t extensionCount = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));

    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()));

    bool hasVulkan = false;
    for (const auto& ext : extensions) {
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) hasVulkan = true;
        if (strcmp(ext.extensionName, XR_EXT_MACOS_WINDOW_BINDING_EXTENSION_NAME) == 0) {
            xr.hasMacosWindowBinding = true;
        }
        if (strcmp(ext.extensionName, XR_EXT_DISPLAY_INFO_EXTENSION_NAME) == 0) {
            xr.hasDisplayInfoExt = true;
        }
    }

    if (!hasVulkan) {
        LOG_ERROR("XR_KHR_vulkan_enable not supported");
        return false;
    }

    LOG_INFO("XR_EXT_macos_window_binding: %s", xr.hasMacosWindowBinding ? "available" : "not available");
    LOG_INFO("XR_EXT_display_info: %s", xr.hasDisplayInfoExt ? "available" : "not available");

    // Build extension list
    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (xr.hasMacosWindowBinding) {
        enabledExtensions.push_back(XR_EXT_MACOS_WINDOW_BINDING_EXTENSION_NAME);
    }
    if (xr.hasDisplayInfoExt) {
        enabledExtensions.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);
    }

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(createInfo.applicationInfo.applicationName, "SimCubeExtMacOS", XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.applicationVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    createInfo.enabledExtensionNames = enabledExtensions.data();

    XR_CHECK(xrCreateInstance(&createInfo, &xr.instance));
    LOG_INFO("OpenXR instance created");

    XrSystemGetInfo sysInfo = {XR_TYPE_SYSTEM_GET_INFO};
    sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(xr.instance, &sysInfo, &xr.systemId));
    LOG_INFO("Got system ID: %llu", (unsigned long long)xr.systemId);

    // Get system name
    {
        XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sysProps))) {
            memcpy(xr.systemName, sysProps.systemName, sizeof(xr.systemName));
            LOG_INFO("System name: %s", xr.systemName);
        }
    }

    // Query display info via XR_EXT_display_info
    if (xr.hasDisplayInfoExt) {
        XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayInfoEXT displayInfo = {};
        displayInfo.type = XR_TYPE_DISPLAY_INFO_EXT;
        sysProps.next = &displayInfo;
        if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sysProps))) {
            xr.recommendedViewScaleX = displayInfo.recommendedViewScaleX;
            xr.recommendedViewScaleY = displayInfo.recommendedViewScaleY;
            xr.displayWidthM = displayInfo.displaySizeMeters.width;
            xr.displayHeightM = displayInfo.displaySizeMeters.height;
            xr.nominalViewerX = displayInfo.nominalViewerPositionInDisplaySpace.x;
            xr.nominalViewerY = displayInfo.nominalViewerPositionInDisplaySpace.y;
            xr.nominalViewerZ = displayInfo.nominalViewerPositionInDisplaySpace.z;
            xr.supportsDisplayModeSwitch = displayInfo.supportsDisplayModeSwitch;
            xr.displayPixelWidth = displayInfo.displayPixelWidth;
            xr.displayPixelHeight = displayInfo.displayPixelHeight;
            LOG_INFO("Display pixels: %ux%u", xr.displayPixelWidth, xr.displayPixelHeight);
            LOG_INFO("Display info: %.3fx%.3f m, scale=%.2fx%.2f, nominal=(%.3f,%.3f,%.3f)",
                xr.displayWidthM, xr.displayHeightM,
                xr.recommendedViewScaleX, xr.recommendedViewScaleY,
                xr.nominalViewerX, xr.nominalViewerY, xr.nominalViewerZ);
            LOG_INFO("Display mode switch: %s", xr.supportsDisplayModeSwitch ? "supported" : "not supported");
        }

        // Load display mode request function pointer
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayModeEXT",
            (PFN_xrVoidFunction*)&xr.pfnRequestDisplayModeEXT);
    }

    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr));
    xr.configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType,
        viewCount, &viewCount, xr.configViews.data()));

    for (uint32_t i = 0; i < viewCount; i++) {
        LOG_INFO("  View %u: %ux%u", i,
            xr.configViews[i].recommendedImageRectWidth,
            xr.configViews[i].recommendedImageRectHeight);
    }

    return true;
}

static bool GetVulkanGraphicsRequirements(AppXrSession& xr) {
    PFN_xrGetVulkanGraphicsRequirementsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&pfn));

    XrGraphicsRequirementsVulkanKHR graphicsReq = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    XR_CHECK(pfn(xr.instance, xr.systemId, &graphicsReq));
    return true;
}

static bool CreateVulkanInstance(AppXrSession& xr, VkInstance& vkInstance) {
    PFN_xrGetVulkanInstanceExtensionsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanInstanceExtensionsKHR",
        (PFN_xrVoidFunction*)&pfn));

    uint32_t bufferSize = 0;
    pfn(xr.instance, xr.systemId, 0, &bufferSize, nullptr);
    std::string extensionsStr(bufferSize, '\0');
    pfn(xr.instance, xr.systemId, bufferSize, &bufferSize, extensionsStr.data());

    std::vector<std::string> extensionStorage;
    std::vector<const char*> extensionPtrs;
    size_t start = 0;
    for (size_t i = 0; i <= extensionsStr.size(); i++) {
        if (i == extensionsStr.size() || extensionsStr[i] == ' ' || extensionsStr[i] == '\0') {
            if (i > start) {
                extensionStorage.push_back(extensionsStr.substr(start, i - start));
                extensionPtrs.push_back(extensionStorage.back().c_str());
            }
            start = i + 1;
        }
    }

    // Check for portability enumeration
    uint32_t availCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &availCount, nullptr);
    std::vector<VkExtensionProperties> availExts(availCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &availCount, availExts.data());

    bool hasPortabilityEnum = false;
    for (const auto& e : availExts) {
        if (strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
            hasPortabilityEnum = true;
            extensionPtrs.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            break;
        }
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "SimCubeExtMacOS";
    appInfo.apiVersion = VK_API_VERSION_1_0;

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
    g_physDevice = physDevice;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physDevice, &props);
    LOG_INFO("Vulkan physical device: %s", props.deviceName);
    return true;
}

static bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &count, families.data());

    for (uint32_t i = 0; i < count; i++) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queueFamilyIndex = i;
            return true;
        }
    }
    LOG_ERROR("No graphics queue family found");
    return false;
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
    std::string extStr(bufferSize, '\0');
    pfn(xr.instance, xr.systemId, bufferSize, &bufferSize, extStr.data());

    // Enumerate available device extensions
    uint32_t availCount = 0;
    vkEnumerateDeviceExtensionProperties(physDevice, nullptr, &availCount, nullptr);
    std::vector<VkExtensionProperties> availExts(availCount);
    vkEnumerateDeviceExtensionProperties(physDevice, nullptr, &availCount, availExts.data());

    auto isAvailable = [&](const char *name) -> bool {
        for (const auto& e : availExts) {
            if (strcmp(e.extensionName, name) == 0) return true;
        }
        return false;
    };

    // Phase 1: Build extensionStorage (all strings) first to avoid
    // dangling c_str() pointers from vector reallocation.
    extensionStorage.clear();
    size_t start = 0;
    for (size_t i = 0; i <= extStr.size(); i++) {
        if (i == extStr.size() || extStr[i] == ' ' || extStr[i] == '\0') {
            if (i > start) {
                std::string ext = extStr.substr(start, i - start);
                if (isAvailable(ext.c_str())) {
                    extensionStorage.push_back(ext);
                } else {
                    LOG_WARN("  Device ext UNAVAILABLE (skipping): %s", ext.c_str());
                }
            }
            start = i + 1;
        }
    }

    // Add optional extensions if available
    for (const auto& e : availExts) {
        if (strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) {
            extensionStorage.push_back("VK_KHR_portability_subset");
        }
        if (strcmp(e.extensionName, VK_KHR_MAINTENANCE1_EXTENSION_NAME) == 0) {
            extensionStorage.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
        }
    }

    // Phase 2: Build deviceExtensions pointers from final extensionStorage
    // (safe now since extensionStorage won't be modified again)
    for (const auto& name : extensionStorage) {
        deviceExtensions.push_back(name.c_str());
        LOG_INFO("  Device ext: %s", name.c_str());
    }

    return true;
}

static bool CreateVulkanDevice(VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    const std::vector<const char*>& deviceExtensions, VkDevice& device, VkQueue& graphicsQueue)
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
    LOG_INFO("Vulkan device created");
    return true;
}

/*!
 * Create OpenXR session WITH the macos_window_binding extension.
 * Chains XrMacOSWindowBindingCreateInfoEXT into session create info.
 */
static bool CreateSession(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex)
{
    LOG_INFO("Creating OpenXR session with Vulkan binding + macOS window binding...");

    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = vkInstance;
    vkBinding.physicalDevice = physDevice;
    vkBinding.device = device;
    vkBinding.queueFamilyIndex = queueFamilyIndex;
    vkBinding.queueIndex = 0;

    // Chain the macOS window binding extension — pass our NSView to the runtime
    XrMacOSWindowBindingCreateInfoEXT macosBinding = {};
    macosBinding.type = XR_TYPE_MACOS_WINDOW_BINDING_CREATE_INFO_EXT;
    macosBinding.next = nullptr;
    macosBinding.viewHandle = (__bridge void *)g_metalView;

    // Chain: sessionInfo -> vkBinding -> macosBinding
    if (xr.hasMacosWindowBinding) {
        vkBinding.next = &macosBinding;
        LOG_INFO("Chaining XR_EXT_macos_window_binding with NSView %p", macosBinding.viewHandle);
    }

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &vkBinding;
    sessionInfo.systemId = xr.systemId;

    XR_CHECK(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
    LOG_INFO("Session created%s", xr.hasMacosWindowBinding ? " (with external window)" : "");

    return true;
}

static bool CreateSpaces(AppXrSession& xr) {
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

    // DISPLAY space: physically anchored, unaffected by recentering
    XrReferenceSpaceCreateInfo displaySpaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    displaySpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_DISPLAY_EXT;
    displaySpaceInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
    displaySpaceInfo.poseInReferenceSpace.position = {0, 0, 0};
    XrResult dispResult = xrCreateReferenceSpace(xr.session, &displaySpaceInfo, &xr.displaySpace);
    if (XR_SUCCEEDED(dispResult)) {
        LOG_INFO("DISPLAY space created");
    } else {
        LOG_INFO("DISPLAY space not available, using LOCAL as fallback");
        xr.displaySpace = XR_NULL_HANDLE;
    }

    LOG_INFO("Reference spaces created");
    return true;
}

static bool CreateSwapchains(AppXrSession& xr) {
    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, formatCount, &formatCount, formats.data()));

    int64_t selectedFormat = formats[0];
    LOG_INFO("Selected swapchain format: %lld", (long long)selectedFormat);

    // Single SBS swapchain at native display resolution
    uint32_t scWidth = xr.displayPixelWidth;
    uint32_t scHeight = xr.displayPixelHeight;
    if (scWidth == 0 || scHeight == 0) {
        scWidth = xr.configViews[0].recommendedImageRectWidth * 2;
        scHeight = xr.configViews[0].recommendedImageRectHeight;
    }
    // Cap at max
    if (scWidth > xr.configViews[0].maxImageRectWidth) scWidth = xr.configViews[0].maxImageRectWidth;
    if (scHeight > xr.configViews[0].maxImageRectHeight) scHeight = xr.configViews[0].maxImageRectHeight;

    XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.format = selectedFormat;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = scWidth;
    swapchainInfo.height = scHeight;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;

    XR_CHECK(xrCreateSwapchain(xr.session, &swapchainInfo, &xr.swapchain.swapchain));
    xr.swapchain.format = selectedFormat;
    xr.swapchain.width = scWidth;
    xr.swapchain.height = scHeight;

    uint32_t imageCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(xr.swapchain.swapchain, 0, &imageCount, nullptr));
    xr.swapchain.imageCount = imageCount;
    LOG_INFO("Swapchain: %ux%u, %u images", scWidth, scHeight, imageCount);

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
    if (XR_FAILED(result)) { xr.exitRequested = true; return false; }

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

static bool EndFrame(AppXrSession& xr, XrTime displayTime,
    const XrCompositionLayerProjectionView* views, uint32_t viewCount = 2) {
    XrCompositionLayerProjection projectionLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    // Use DISPLAY space when available (physically anchored)
    projectionLayer.space = (xr.displaySpace != XR_NULL_HANDLE) ? xr.displaySpace : xr.localSpace;
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
    }
    if (xr.displaySpace != XR_NULL_HANDLE) xrDestroySpace(xr.displaySpace);
    if (xr.viewSpace != XR_NULL_HANDLE) xrDestroySpace(xr.viewSpace);
    if (xr.localSpace != XR_NULL_HANDLE) xrDestroySpace(xr.localSpace);
    if (xr.session != XR_NULL_HANDLE) xrDestroySession(xr.session);
    if (xr.instance != XR_NULL_HANDLE) xrDestroyInstance(xr.instance);
}

// ============================================================================
// Main
// ============================================================================

static void SignalHandler(int sig) {
    (void)sig;
    g_running = false;
}

int main() {
    // Ensure logs appear immediately even when piped
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    LOG_INFO("=== Sim Cube OpenXR + External macOS Window ===");

    // Initialize output mode from env var (matches runtime's parsing).
    {
        const char *mode_str = getenv("SIM_DISPLAY_OUTPUT");
        if (mode_str) {
            if (strcmp(mode_str, "anaglyph") == 0)
                g_currentOutputMode = 1;
            else if (strcmp(mode_str, "blend") == 0)
                g_currentOutputMode = 2;
            else
                g_currentOutputMode = 0;
        }
    }

    // Step 1: Create the macOS window FIRST (app owns it)
    g_windowW = 1280;
    g_windowH = 720;
    if (!CreateMacOSWindow(g_windowW, g_windowH)) {
        LOG_ERROR("Failed to create macOS window");
        return 1;
    }

    // Update g_windowW/H to actual drawable pixel dimensions (Retina-aware).
    // The initial 1280x720 are in points; the Vulkan surface uses pixel dimensions.
    {
        NSSize contentSize = [[g_window contentView] bounds].size;
        CGFloat backingScale = [g_window backingScaleFactor];
        g_windowW = (uint32_t)(contentSize.width * backingScale);
        g_windowH = (uint32_t)(contentSize.height * backingScale);
        LOG_INFO("Window drawable size: %ux%u (points: %.0fx%.0f, scale: %.1f)",
                 g_windowW, g_windowH, contentSize.width, contentSize.height, (float)backingScale);
    }

    // Step 2: Initialize OpenXR
    AppXrSession xr = {};
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        return 1;
    }

    // Try to find sim_display_set_output_mode in the loaded runtime (1/2/3 keys).
    // OpenXR loader dlopen's the runtime with RTLD_LOCAL, so RTLD_DEFAULT won't see it.
    // Use _dyld APIs to find the loaded runtime library and get a proper handle.
    {
        void *rtHandle = NULL;
        uint32_t imageCount = _dyld_image_count();
        for (uint32_t i = 0; i < imageCount; i++) {
            const char *name = _dyld_get_image_name(i);
            if (name && strstr(name, "openxr_monado")) {
                LOG_INFO("Found runtime image: %s", name);
                rtHandle = dlopen(name, RTLD_NOLOAD);
                break;
            }
        }
        if (rtHandle) {
            g_pfnSetOutputMode = (PFN_sim_display_set_output_mode)dlsym(rtHandle, "sim_display_set_output_mode");
        }
        if (g_pfnSetOutputMode) {
            LOG_INFO("sim_display hot-reload: available");
        } else {
            LOG_INFO("sim_display hot-reload: not available (%s)", dlerror());
        }
    }

    if (!GetVulkanGraphicsRequirements(xr)) {
        CleanupOpenXR(xr);
        return 1;
    }

    // Step 3: Create Vulkan instance + device
    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) {
        CleanupOpenXR(xr);
        return 1;
    }

    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    std::vector<const char*> deviceExtensions;
    std::vector<std::string> extensionStorage;
    if (!GetVulkanDeviceExtensions(xr, deviceExtensions, extensionStorage, physDevice)) {
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }
    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, deviceExtensions, vkDevice, graphicsQueue)) {
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    // Step 4: Create OpenXR session WITH the external window binding
    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex)) {
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    if (!CreateSpaces(xr) || !CreateSwapchains(xr)) {
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        return 1;
    }

    // Step 5: Enumerate swapchain images + init renderer
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
    }

    VkFormat colorFormat = (VkFormat)xr.swapchain.format;
    VkRenderer vkRenderer = {};
    if (!InitializeVkRenderer(vkRenderer, vkDevice, physDevice, graphicsQueue, queueFamilyIndex, colorFormat)) {
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        return 1;
    }

    {
        uint32_t count = xr.swapchain.imageCount;
        LOG_INFO("Creating framebuffers: %u images, %ux%u", count, xr.swapchain.width, xr.swapchain.height);
        std::vector<VkImage> images(count);
        for (uint32_t i = 0; i < count; i++) {
            images[i] = swapchainImages[i].image;
        }
        if (!CreateSwapchainFramebuffers(vkRenderer, images.data(), count,
            xr.swapchain.width, xr.swapchain.height, colorFormat)) {
            CleanupVkRenderer(vkRenderer);
            CleanupOpenXR(xr);
            vkDestroyDevice(vkDevice, nullptr);
            vkDestroyInstance(vkInstance, nullptr);
            return 1;
        }
        LOG_INFO("Framebuffers created OK");
    }

    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: WASD=move, Mouse=look, Scroll=zoom, Space=reset, V=2D/3D, 1/2/3=SBS/anaglyph/blend, TAB=HUD, Cmd+Ctrl+F=fullscreen, ESC=quit");
    LOG_INFO("          V=2D/3D, Tab=HUD, 1/2/3=SBS/Ana/Blend, ESC=quit");

    // Known issue: during window resize, [NSApp sendEvent:] enters a modal
    // tracking loop that blocks this while loop, freezing animation and frame
    // submission until the mouse is released. Two approaches were tried:
    //
    //  1. CFRunLoopTimer on kCFRunLoopCommonModes — fires during resize
    //     tracking but caused "Unknown failure" from the OpenXR loader
    //     (C++ exception escaping from the Monado runtime).
    //
    //  2. Dedicated render pthread — same "Unknown failure". The Monado
    //     runtime throws when OpenXR frame-loop calls (xrWaitFrame /
    //     xrBeginFrame / xrEndFrame) run on a thread other than the one
    //     that created the session. Even starting the session on the main
    //     thread first did not help.
    //
    // The simple while-loop works reliably; resize just pauses animation.

    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !xr.exitRequested) {
        PumpMacOSEvents();

        auto now = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        // Performance stats
        g_frameCount++;
        g_avgFrameTime = g_avgFrameTime * 0.95 + deltaTime * 0.05;

        UpdateCameraMovement(g_input, deltaTime);

        vkRenderer.cubeRotation += deltaTime * 0.5f;
        if (vkRenderer.cubeRotation > 2.0f * 3.14159265f)
            vkRenderer.cubeRotation -= 2.0f * 3.14159265f;

        // Handle display mode toggle (V key)
        if (g_input.displayModeToggleRequested) {
            g_input.displayModeToggleRequested = false;
            if (xr.pfnRequestDisplayModeEXT && xr.session != XR_NULL_HANDLE) {
                XrDisplayModeEXT mode = g_input.displayMode3D
                    ? XR_DISPLAY_MODE_3D_EXT : XR_DISPLAY_MODE_2D_EXT;
                XrResult modeResult = xr.pfnRequestDisplayModeEXT(xr.session, mode);
                LOG_INFO("Display mode → %s (%s)", g_input.displayMode3D ? "3D" : "2D",
                    XR_SUCCEEDED(modeResult) ? "OK" : "failed");
            }
        }

        PollEvents(xr);

        if (xr.sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(xr, frameState)) {
                XrCompositionLayerProjectionView projectionViews[2] = {};
                bool rendered = false;

                if (frameState.shouldRender) {
                    // Locate views in DISPLAY space (physically anchored) or LOCAL fallback
                    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                    locateInfo.viewConfigurationType = xr.viewConfigType;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = (xr.displaySpace != XR_NULL_HANDLE)
                        ? xr.displaySpace : xr.localSpace;

                    XrViewState viewState = {XR_TYPE_VIEW_STATE};
                    uint32_t viewCount = 2;
                    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};

                    XrResult locResult = xrLocateViews(xr.session, &locateInfo,
                        &viewState, 2, &viewCount, views);
                    if (XR_SUCCEEDED(locResult) &&
                        (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
                    {
                        // Save raw display-space positions for Kooima + HUD
                        XrVector3f rawEyePos[2] = {views[0].pose.position, views[1].pose.position};

                        // Store raw eye positions for HUD (pre-player-transform)
                        xr.leftEyeX = rawEyePos[0].x; xr.leftEyeY = rawEyePos[0].y; xr.leftEyeZ = rawEyePos[0].z;
                        xr.rightEyeX = rawEyePos[1].x; xr.rightEyeY = rawEyePos[1].y; xr.rightEyeZ = rawEyePos[1].z;
                        xr.eyeTrackingActive = (viewState.viewStateFlags
                            & XR_VIEW_STATE_POSITION_TRACKED_BIT) != 0;

                        // Determine eye count (mono in 2D, stereo in 3D)
                        int eyeCount = g_input.displayMode3D ? 2 : 1;

                        // In mono mode, use center eye (average of L/R)
                        if (!g_input.displayMode3D) {
                            rawEyePos[0] = {
                                (rawEyePos[0].x + rawEyePos[1].x) / 2.0f,
                                (rawEyePos[0].y + rawEyePos[1].y) / 2.0f,
                                (rawEyePos[0].z + rawEyePos[1].z) / 2.0f};
                            rawEyePos[1] = rawEyePos[0];
                        }

                        // Build display pose from player transform
                        XrPosef displayPose;
                        quat_from_yaw_pitch(g_input.yaw, g_input.pitch, &displayPose.orientation);
                        displayPose.position = {g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ};

                        // Build tunables from stereo params
                        Display3DTunables tunables;
                        tunables.ipd_factor = g_input.stereo.ipdFactor;
                        tunables.parallax_factor = g_input.stereo.parallaxFactor;
                        tunables.perspective_factor = g_input.stereo.perspectiveFactor;
                        tunables.scale_factor = g_input.stereo.scaleFactor;

                        XrVector3f nominalViewer = {xr.nominalViewerX, xr.nominalViewerY, xr.nominalViewerZ};

                        // Compute render dims for SBS single-swapchain.
                        // Scale depends on current output mode (may change at runtime):
                        //   SBS (0):          scaleX=0.5, scaleY=1.0
                        //   Anaglyph/Blend:   scaleX=0.5, scaleY=0.5
                        float scaleX = 0.5f;
                        float scaleY = (g_currentOutputMode == 0) ? 1.0f : 0.5f;
                        xr.recommendedViewScaleX = scaleX;
                        xr.recommendedViewScaleY = scaleY;

                        uint32_t eyeRenderW = xr.swapchain.width / 2;
                        uint32_t eyeRenderH = xr.swapchain.height;
                        uint32_t renderW, renderH;
                        if (!g_input.displayMode3D) {
                            renderW = g_windowW;
                            renderH = g_windowH;
                            if (renderW > xr.swapchain.width) renderW = xr.swapchain.width;
                            if (renderH > xr.swapchain.height) renderH = xr.swapchain.height;
                        } else {
                            renderW = (uint32_t)(g_windowW * scaleX);
                            renderH = (uint32_t)(g_windowH * scaleY);
                            if (renderW > eyeRenderW) renderW = eyeRenderW;
                            if (renderH > eyeRenderH) renderH = eyeRenderH;
                        }
                        g_renderW = renderW;
                        g_renderH = renderH;

                        // Compute stereo views using display3d library
                        Display3DStereoView stereoViews[2];
                        bool hasKooima = (xr.displayWidthM > 0 && xr.displayHeightM > 0);
                        if (hasKooima) {
                            // Compute viewport-scaled screen dimensions in meters
                            float dispPxW = xr.displayPixelWidth > 0 ? (float)xr.displayPixelWidth : (float)xr.swapchain.width;
                            float dispPxH = xr.displayPixelHeight > 0 ? (float)xr.displayPixelHeight : (float)xr.swapchain.height;
                            float pxSizeX = xr.displayWidthM / dispPxW;
                            float pxSizeY = xr.displayHeightM / dispPxH;
                            float winW_m = (float)g_windowW * pxSizeX;
                            float winH_m = (float)g_windowH * pxSizeY;
                            float minDisp = fminf(xr.displayWidthM, xr.displayHeightM);
                            float minWin  = fminf(winW_m, winH_m);
                            float vs = minDisp / minWin;
                            float screenWidthM  = winW_m * vs;
                            float screenHeightM = winH_m * vs;
                            // Halve screenW only for SBS mode (each eye sees
                            // half the display). Anaglyph/blend: full width.
                            bool sbsMode = g_input.displayMode3D && g_currentOutputMode == 0;
                            float baseScreenW = sbsMode ? screenWidthM / 2.0f : screenWidthM;

                            Display3DScreen screen;
                            screen.width_m = baseScreenW;
                            screen.height_m = screenHeightM;

                            display3d_compute_stereo_views(
                                &rawEyePos[0], &rawEyePos[1], &nominalViewer,
                                &screen, &tunables, &displayPose,
                                0.01f, 100.0f, &stereoViews[0], &stereoViews[1]);
                        }

                        rendered = true;
                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(xr, imageIndex)) {
                            EyeRenderParams eyeParams[2];
                            for (int eye = 0; eye < eyeCount; eye++) {
                                XrFovf submitFov = views[eye].fov;
                                if (hasKooima) {
                                    memcpy(eyeParams[eye].viewMat, stereoViews[eye].view_matrix, sizeof(float) * 16);
                                    memcpy(eyeParams[eye].projMat, stereoViews[eye].projection_matrix, sizeof(float) * 16);
                                    submitFov = stereoViews[eye].fov;
                                    // Update view pose for layer submission
                                    views[eye].pose.position = stereoViews[eye].eye_world;
                                    views[eye].pose.orientation = displayPose.orientation;
                                } else {
                                    mat4_view_from_xr_pose(eyeParams[eye].viewMat, views[eye].pose);
                                    mat4_from_xr_fov(eyeParams[eye].projMat, views[eye].fov, 0.01f, 100.0f);
                                }

                                uint32_t vpX = g_input.displayMode3D ? (eye * renderW) : 0;
                                eyeParams[eye].viewportX = vpX;
                                eyeParams[eye].viewportY = 0;
                                eyeParams[eye].width = renderW;
                                eyeParams[eye].height = renderH;

                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {(int32_t)vpX, 0};
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)renderW,
                                    (int32_t)renderH};
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                projectionViews[eye].pose = views[eye].pose;
                                projectionViews[eye].fov = submitFov;
                            }
                            RenderScene(vkRenderer, imageIndex, eyeParams, eyeCount);
                            ReleaseSwapchainImage(xr);
                        } else {
                            rendered = false;
                        }
                    }
                }

                if (rendered) {
                    uint32_t submitCount = g_input.displayMode3D ? 2u : 1u;
                    EndFrame(xr, frameState.predictedDisplayTime, projectionViews, submitCount);
                } else {
                    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                    endInfo.displayTime = frameState.predictedDisplayTime;
                    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    endInfo.layerCount = 0;
                    endInfo.layers = nullptr;
                    xrEndFrame(xr.session, &endInfo);
                }
            }
        } else {
            usleep(100000);
        }

        // Update HUD overlay (throttled)
        g_hudUpdateTimer += deltaTime;
        if (g_hudUpdateTimer >= 0.5f) {
            g_hudUpdateTimer = 0.0f;
            @autoreleasepool {
                if (g_input.hudVisible && g_hudView != nil) {
                    double fps = (g_avgFrameTime > 0) ? 1.0 / g_avgFrameTime : 0;
                    const char *outputModeNames[] = {"SBS", "Anaglyph", "Blend"};
                    const char *outputModeName = (g_currentOutputMode >= 0 && g_currentOutputMode <= 2)
                        ? outputModeNames[g_currentOutputMode] : "?";
                    // Session state name lookup
                    const char *sessionStateNames[] = {
                        "UNKNOWN", "IDLE", "READY", "SYNCHRONIZED",
                        "VISIBLE", "FOCUSED", "STOPPING", "LOSS_PENDING", "EXITING"};
                    int stateIdx = (int)xr.sessionState;
                    const char *sessionStateName = (stateIdx >= 0 && stateIdx < 9)
                        ? sessionStateNames[stateIdx] : "INVALID";

                    NSString *text = [NSString stringWithFormat:
                        @"%s\n"
                        "Session: %s\n"
                        "XR_EXT_macos_window_binding: %s\n"
                        "Mode: %s%s  Output: %s\n"
                        "FPS: %.0f  (%.1f ms)\n"
                        "Render: %ux%u\n"
                        "Window: %ux%u\n"
                        "Display: %.3f x %.3f m\n"
                        "Scale: %.2f x %.2f\n"
                        "Nominal: (%.3f, %.3f, %.3f)\n"
                        "Eye L: (%.3f, %.3f, %.3f)\n"
                        "Eye R: (%.3f, %.3f, %.3f)\n"
                        "Virtual Display: (%.2f, %.2f, %.2f)\n"
                        "Forward: (%.2f, %.2f, %.2f)\n"
                        "IPD: %.2f  Parallax: %.2f\n"
                        "Persp: %.2f  Scale: %.2f\n"
                        "\n"
                        "WASD/QE=Move  Drag=Look  Space=Reset\n"
                        "Scroll=Scale  Shift=IPD  Ctrl=Parallax  Opt=Persp\n"
                        "V=2D/3D  1/2/3=Output  Tab=HUD  ESC=Quit",
                        xr.systemName,
                        sessionStateName,
                        xr.hasMacosWindowBinding ? "ACTIVE" : "NOT AVAILABLE",
                        g_input.displayMode3D ? "3D (Stereo)" : "2D (Mono)",
                        xr.supportsDisplayModeSwitch ? "" : " [no switch]",
                        outputModeName,
                        fps, g_avgFrameTime * 1000.0,
                        g_renderW, g_renderH,
                        g_windowW, g_windowH,
                        xr.displayWidthM, xr.displayHeightM,
                        xr.recommendedViewScaleX, xr.recommendedViewScaleY,
                        xr.nominalViewerX, xr.nominalViewerY, xr.nominalViewerZ,
                        xr.leftEyeX, xr.leftEyeY, xr.leftEyeZ,
                        xr.rightEyeX, xr.rightEyeY, xr.rightEyeZ,
                        g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ,
                        -sinf(g_input.yaw) * cosf(g_input.pitch),
                         sinf(g_input.pitch),
                        -cosf(g_input.yaw) * cosf(g_input.pitch),
                        g_input.stereo.ipdFactor, g_input.stereo.parallaxFactor,
                        g_input.stereo.perspectiveFactor, g_input.stereo.scaleFactor];
                    g_hudView.hudText = text;
                    [g_hudView setNeedsDisplay:YES];
                    [g_hudView setHidden:NO];
                } else if (g_hudView != nil) {
                    [g_hudView setHidden:YES];
                }
            }
        }
    }

    // Clean exit
    if (!xr.exitRequested && xr.session != XR_NULL_HANDLE && xr.sessionRunning) {
        LOG_INFO("Requesting session exit...");
        xrRequestExitSession(xr.session);
        for (int i = 0; i < 100 && !xr.exitRequested; i++) {
            PollEvents(xr);
            usleep(10000);
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
