// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Cube Zones TEXTURE OpenGL — XR_EXT_display_zones parity test (ADR-027), macOS leg
 *
 * PARITY TEST. This is the OpenGL sibling of cube_zones_texture_metal_macos. It
 * DRIVES the runtime GL native-compositor IOSurface zones-present path (the
 * `#ifdef __APPLE__` block in src/xrt/compositor/gl/comp_gl_compositor.cpp gated
 * on `c->has_shared_iosurface`): a GL texture app provides a shared IOSurface
 * and presents that surface itself, and receives the FULL XR_EXT_display_zones
 * multi-zone composite written back into its shared IOSurface — byte-identical
 * to what a handle app gets composited into its window. The display-zones
 * submission logic (the thing under test) is unchanged from the Metal leg.
 *
 * Metal -> GL deltas (everything else — zones / Local2D / wish-mask /
 * cocoa-binding+sharedIOSurface / IOSurface sizing / main loop / events / HUD /
 * MCP / IOSurface->PNG readback — is identical to the Metal leg):
 *  - GL graphics binding: XrGraphicsBindingOpenGLMacOSEXT (cglContext from the
 *    NSOpenGLContext), with XrCocoaWindowBindingCreateInfoEXT (carrying the
 *    on-screen NSView + the shared IOSurface) chained on `.next`, plus
 *    xrGetOpenGLGraphicsRequirementsKHR. XR_EXT_macos_gl_binding is REQUIRED.
 *  - NSOpenGLView / NSOpenGLContext (GL 4.1 Core) instead of a CAMetalLayer.
 *  - GLSL + FBO rendering instead of MSL render passes; matrices use the GL
 *    [-1,1] clip convention.
 *  - Present: the app maps the runtime-composited IOSurface as a
 *    GL_TEXTURE_RECTANGLE (CGLTexImageIOSurface2D) and blits it to the default
 *    framebuffer, then flushBuffer.
 *
 * Texture-mode deltas from a handle app:
 *  - Creates a shared IOSurface sized to the worst-case atlas (same sizing as
 *    cube_texture_metal_macos) and chains it as
 *    XrCocoaWindowBindingCreateInfoEXT.sharedIOSurface on xrCreateSession.
 *  - The APP owns presentation: each frame it blits the IOSurface into its own
 *    GL default framebuffer and presents. The runtime does NOT present.
 *  - When a zones frame is active the GL compositor composites the full-
 *    window multi-zone super-atlas DIRECTLY into the shared IOSurface (the
 *    declared output rect is superseded by zones → full window), so NO
 *    xrSetSharedTextureOutputRectEXT / 2D-surround is needed for the zones
 *    path. The app simply binds the IOSurface, submits zones, presents.
 *  - Autonomous verification: it reads the IOSurface back to a PNG via
 *    stbi_write_png after a warmup gate (~frame 150) so the captured surface
 *    reflects what the texture app actually received (see DXR_TEXDUMP and the
 *    /tmp/dxr_atlas_trigger file-trigger below).
 *
 * Exercises the display-zones runtime path (N 3D zones + Local2D zones +
 * per-frame wish mask) end to end on the OpenGL native compositor:
 *
 *  - Zone A (zoneId=1, left)  : left half below the strip, identity display
 *    rig, spin phase 0, dark-red clear.
 *  - Zone B (zoneId=2, right) : right side, display rig with ipdFactor 0.6 +
 *    perspectiveFactor 0.5 (visibly different framing), spin phase +1.5 rad,
 *    SEMI-TRANSPARENT dark-blue clear (alpha 0.55, premultiplied) so the
 *    O-key overlap visibly alpha-overs zone A.
 *  - Local2D strip (top 25%)  : always on, filled once with a CPU-generated
 *    checker + amber label band.
 *
 * Rects are computed from the live window backing size at activation (the
 * D3D11 source app uses the same proportions of its fixed 1280x720 window).
 * Each zone owns ONE swapchain sized per xrGetDisplayZoneRecommendedViewSizeEXT,
 * horizontally tiled per view; each frame runs a zone-scoped locate
 * (XrDisplayZoneEXT + XrDisplayRigEXT chained on XrViewLocateInfo) and submits
 * [projA, projB, strip] with the SAME zone structs chained on the projections.
 *
 * Keys (zones mode):
 *  - M : cycle wish mode — 0 AUTO (no frame-end info) / 1 explicit Tier-2
 *        rects. (Tier-3 freeform render-target wish has no Metal binding —
 *        XrLocal3DZoneRenderTarget*EXT exists for D3D11/D3D12/VK only.)
 *  - O : toggle zone B between its home rect and a rect overlapping zone A
 *        (locate + submit always share the one rect variable).
 *  - DXR_ZONES_VALIDATE=1 : chain XR_DISPLAY_ZONES_FRAME_END_VALIDATE_BIT_EXT
 *        on the frame-end info in every mode (one-shot runtime WARNs).
 *
 * When the runtime doesn't advertise XR_EXT_display_zones the app logs an
 * error once and keeps running as the plain single-projection cube
 * (graceful degrade — the whole base-app path is intact below).
 */

#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>
#import <IOSurface/IOSurface.h>
#import <IOSurface/IOSurfaceObjC.h>
#import <QuartzCore/QuartzCore.h>   // needed by displayxr::common atlas_capture flash helper

#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_cocoa_window_binding.h>
#include <openxr/XR_EXT_macos_gl_binding.h>

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>
#include <vector>

#include <unistd.h>
#include <mach-o/dyld.h>

// stb_image implementation TU lives in displayxr::common (stb_image_impl_macos.cpp) — declarations only here (#396 W4).
#include "stb_image.h"
// stb_image_write is NOT exposed by displayxr::common's stb header; forward-
// declare the one entry point we need for the IOSurface readback dump (see the
// "macOS test-app pixels" guidance in CLAUDE.md + reference_macos_test_app_capture).
// The implementation is provided by displayxr::common's stb impl TU.
extern "C" int stbi_write_png(const char *filename, int w, int h, int comp,
                              const void *data, int stride_in_bytes);
#include "view_params.h"
#include "rig_mode.h"
#include "atlas_capture.h"
#include "xr_window_space_hud.h"
#include "hud_renderer_macos.h"
#include <openxr/XR_EXT_display_info.h>
#include <openxr/XR_EXT_local_3d_zone.h>
#include <openxr/XR_EXT_display_zones.h>
#include <openxr/XR_EXT_atlas_capture.h>
#include <openxr/XR_EXT_mcp_tools.h>
#include <openxr/XR_EXT_view_rig.h>

// ============================================================================
// Logging
// ============================================================================

#define LOG_INFO(fmt, ...) fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#define XR_CHECK(call)                                                         \
    do {                                                                       \
        XrResult _r = (call);                                                  \
        if (XR_FAILED(_r)) {                                                   \
            LOG_ERROR("OpenXR error %d at %s:%d", _r, __FILE__, __LINE__);     \
            return false;                                                      \
        }                                                                      \
    } while (0)

// ============================================================================
// XR_EXT_mcp_tools — reference adoption (#447)
//
// This is the canonical in-tree example of an app exposing its own MCP
// tools to agents: declare an appId matching the manifest `id` (the
// agent-visible tool prefix, e.g. cube-zones-texture-gl__set_spin through the
// workspace aggregator), register tools after session create, and
// answer XrEventDataMCPToolCallEXT from the normal event pump. Inert
// when the MCP capability is disabled. Spec:
// docs/specs/extensions/XR_EXT_mcp_tools.md.
// ============================================================================

static bool g_hasMcpToolsExt = false;
static PFN_xrSetMCPAppInfoEXT g_pfnSetMCPAppInfo = nullptr;
static PFN_xrRegisterMCPToolEXT g_pfnRegisterMCPTool = nullptr;
static PFN_xrGetMCPToolCallArgsEXT g_pfnGetMCPToolCallArgs = nullptr;
static PFN_xrSubmitMCPToolResultEXT g_pfnSubmitMCPToolResult = nullptr;

//! Cube spin speed in rad/s — historically hardcoded 0.5; now agent-settable.
static float g_spinSpeed = 0.5f;

// ============================================================================
// Math (column-major 4x4 matrices)
// ============================================================================

static void mat4_identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_multiply(float out[16], const float a[16], const float b[16])
{
    float tmp[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            tmp[c * 4 + r] = 0;
            for (int k = 0; k < 4; k++)
                tmp[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
        }
    }
    memcpy(out, tmp, sizeof(tmp));
}

static void mat4_translation(float m[16], float x, float y, float z)
{
    mat4_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
}

static void mat4_scaling(float m[16], float s)
{
    mat4_identity(m);
    m[0] = m[5] = m[10] = s;
}

static void mat4_rotation_y(float m[16], float angle)
{
    mat4_identity(m);
    float c = cosf(angle), s = sinf(angle);
    m[0] = c; m[2] = -s;
    m[8] = s; m[10] = c;
}

static void mat4_from_xr_fov(float m[16], const XrFovf &fov, float nearZ, float farZ)
{
    float l = tanf(fov.angleLeft);
    float r = tanf(fov.angleRight);
    float u = tanf(fov.angleUp);
    float d = tanf(fov.angleDown);

    float w = r - l;
    float h = u - d;

    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f / w;
    m[5]  = 2.0f / h;
    m[8]  = (r + l) / w;
    m[9]  = (u + d) / h;
    // OpenGL uses z range [-1, 1]
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

static void mat4_view_from_xr_pose(float m[16], const XrPosef &pose)
{
    float x = pose.orientation.x, y = pose.orientation.y;
    float z = pose.orientation.z, w = pose.orientation.w;

    float r00 = 1 - 2*(y*y + z*z), r01 = 2*(x*y + w*z),     r02 = 2*(x*z - w*y);
    float r10 = 2*(x*y - w*z),     r11 = 1 - 2*(x*x + z*z), r12 = 2*(y*z + w*x);
    float r20 = 2*(x*z + w*y),     r21 = 2*(y*z - w*x),      r22 = 1 - 2*(x*x + y*y);

    float px = pose.position.x, py = pose.position.y, pz = pose.position.z;

    mat4_identity(m);
    m[0] = r00; m[1] = r10; m[2]  = r20;
    m[4] = r01; m[5] = r11; m[6]  = r21;
    m[8] = r02; m[9] = r12; m[10] = r22;
    m[12] = -(r00*px + r01*py + r02*pz);
    m[13] = -(r10*px + r11*py + r12*pz);
    m[14] = -(r20*px + r21*py + r22*pz);
}

// ============================================================================
// Quaternion helpers (same as cube_handle_vk_macos)
// ============================================================================

static void quat_from_yaw_pitch(float yaw, float pitch, XrQuaternionf* out) {
    float cy = cosf(yaw / 2.0f), sy = sinf(yaw / 2.0f);
    float cp = cosf(pitch / 2.0f), sp = sinf(pitch / 2.0f);
    out->w = cy * cp;
    out->x = cy * sp;
    out->y = sy * cp;
    out->z = -sy * sp;
}

static void quat_rotate_vec3(XrQuaternionf q, float vx, float vy, float vz,
    float* ox, float* oy, float* oz) {
    float tx = 2.0f * (q.y * vz - q.z * vy);
    float ty = 2.0f * (q.z * vx - q.x * vz);
    float tz = 2.0f * (q.x * vy - q.y * vx);
    *ox = vx + q.w * tx + (q.y * tz - q.z * ty);
    *oy = vy + q.w * ty + (q.z * tx - q.x * tz);
    *oz = vz + q.w * tz + (q.x * ty - q.y * tx);
}

// Display-local eye distance for the ZDP-anchored clip (#396 W7 consume
// path): z of (rigPose^-1 * eyeWorld). Degenerates to pose.position.z at
// identity rig pose.
static float RigLocalEyeZ(const XrPosef &rig, const XrVector3f &eyeWorld) {
    XrQuaternionf inv = {-rig.orientation.x, -rig.orientation.y,
                         -rig.orientation.z, rig.orientation.w};
    float ox, oy, oz;
    quat_rotate_vec3(inv,
                     eyeWorld.x - rig.position.x,
                     eyeWorld.y - rig.position.y,
                     eyeWorld.z - rig.position.z,
                     &ox, &oy, &oz);
    return oz;
}

// ============================================================================
// Texture path helper
// ============================================================================

static std::string GetTextureDir()
{
    char path[4096];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        std::string s(path);
        size_t pos = s.find_last_of('/');
        if (pos != std::string::npos)
            return s.substr(0, pos + 1) + "textures/";
    }
    return "textures/";
}

// ============================================================================
// GLSL shaders (OpenGL 4.1 core profile)
// ============================================================================

// Cube vertex shader — uses GL_TEXTURE_RECTANGLE (unnormalized coords).
// Copied verbatim from cube_handle_gl_macos.
static const char *g_cubeVertexShader = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aNormal;
layout(location = 4) in vec3 aTangent;

uniform mat4 uMVP;
uniform mat4 uModel;
uniform vec2 uTexSize; // pixel dimensions for TEXTURE_RECTANGLE

out vec2 vUV;
out vec3 vWorldNormal;
out vec3 vWorldTangent;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV * uTexSize;  // scale [0,1] to pixel coords for TEXTURE_RECTANGLE
    vWorldNormal = (uModel * vec4(aNormal, 0.0)).xyz;
    vWorldTangent = (uModel * vec4(aTangent, 0.0)).xyz;
}
)GLSL";

static const char *g_cubeFragmentShader = R"GLSL(
#version 410 core
uniform sampler2DRect uBasecolorTex;
uniform sampler2DRect uNormalTex;
uniform sampler2DRect uAOTex;

in vec2 vUV;
in vec3 vWorldNormal;
in vec3 vWorldTangent;

out vec4 fragColor;

void main() {
    vec4 baseColor = texture(uBasecolorTex, vUV);
    vec3 normalMap = texture(uNormalTex, vUV).xyz * 2.0 - 1.0;
    float ao = texture(uAOTex, vUV).r;

    vec3 N = normalize(vWorldNormal);
    vec3 T = normalize(vWorldTangent);
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);
    vec3 normal = normalize(TBN * normalMap);

    vec3 lightDir = normalize(vec3(0.3, 0.5, 1.0));
    float diffuse = max(dot(normal, lightDir), 0.0) * 0.8 * ao;
    float ambient = 0.3 + 0.15 * ao;

    fragColor = vec4(baseColor.rgb * (diffuse + ambient), 1.0);
}
)GLSL";

// Grid shaders (verbatim from cube_handle_gl_macos)
static const char *g_gridVertexShader = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

static const char *g_gridFragmentShader = R"GLSL(
#version 410 core
uniform vec4 uColor;
out vec4 fragColor;
void main() {
    fragColor = uColor;
}
)GLSL";

// --- Zone content-alpha edge fade (ADR-027 rule 4) ---
//
// Per ADR-027 rule 4, zone blends are expressed through CONTENT alpha — a
// zone wanting a soft edge fades its OWN rendered alpha at the tile edges.
// Attribute-less fullscreen triangle from gl_VertexID; outputs alpha = (1 - f)
// and is drawn with glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, ...)
// so dst *= (1 - (1 - f)) = f. Ported from cube_zones_texture_gl_win
// (version bumped 330 -> 410 core for the macOS 4.1 context).
static const char *g_fadeVertexShader = R"GLSL(
#version 410 core
out vec2 vUV;
void main() {
    // id 0,1,2 -> a fullscreen triangle covering the [0,1] UV box.
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUV = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

static const char *g_fadeFragmentShader = R"GLSL(
#version 410 core
uniform vec2 uTilePx;     // tile width/height in pixels
uniform vec2 uFeatherPx;  // feather width in pixels (.x; .y unused — glUniform2f)
in vec2 vUV;
out vec4 fragColor;
void main() {
    vec2 px = vUV * uTilePx;
    float d = min(min(px.x, uTilePx.x - px.x), min(px.y, uTilePx.y - px.y));
    float f = clamp(d / uFeatherPx.x, 0.0, 1.0);
    // Blend (ZERO, ONE_MINUS_SRC_ALPHA): dst *= (1 - src.a) = f.
    fragColor = vec4(0.0, 0.0, 0.0, 1.0 - f);
}
)GLSL";

// --- Blit/present shader: fullscreen triangle sampling the shared IOSurface ---
// Texture-mode present: the app maps the runtime-composited IOSurface as a
// GL_TEXTURE_RECTANGLE and blits it to the default framebuffer.
static const char *g_blitVertexShader = R"GLSL(
#version 410 core
uniform vec2 uTexSize; // IOSurface pixel dims, for TEXTURE_RECTANGLE UVs
out vec2 vUV;
void main() {
    // id 0,1,2 -> a fullscreen triangle covering the [0,1] UV box.
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    // PRESENT Y: GL composite is bottom-up; no flip. If inverted at runtime,
    // flip vUV.y here (e.g. vUV = vec2(uv.x, 1.0 - uv.y) * uTexSize).
    vUV = uv * uTexSize; // pixel coords for sampler2DRect
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

static const char *g_blitFragmentShader = R"GLSL(
#version 410 core
uniform sampler2DRect uTex;
in vec2 vUV;
out vec4 fragColor;
void main() {
    fragColor = texture(uTex, vUV);
}
)GLSL";

// ============================================================================
// Vertex data structures
// ============================================================================

struct CubeVertex {
    float pos[3];
    float color[4];
    float uv[2];
    float normal[3];
    float tangent[3];
};

struct GridVertex {
    float pos[3];
};

struct EyeRenderParams {
    uint32_t viewportX, viewportY, width, height;
    float viewMat[16];
    float projMat[16];
};

// ============================================================================
// Cube geometry (24 verts, 36 indices — 6 faces with unique normals)
// ============================================================================

static const CubeVertex g_cubeVertices[] = {
    // Front face (Z+)
    {{-0.5f,-0.5f, 0.5f}, {1,1,1,1}, {0,1}, { 0, 0, 1}, { 1, 0, 0}},
    {{ 0.5f,-0.5f, 0.5f}, {1,1,1,1}, {1,1}, { 0, 0, 1}, { 1, 0, 0}},
    {{ 0.5f, 0.5f, 0.5f}, {1,1,1,1}, {1,0}, { 0, 0, 1}, { 1, 0, 0}},
    {{-0.5f, 0.5f, 0.5f}, {1,1,1,1}, {0,0}, { 0, 0, 1}, { 1, 0, 0}},
    // Back face (Z-)
    {{ 0.5f,-0.5f,-0.5f}, {1,1,1,1}, {0,1}, { 0, 0,-1}, {-1, 0, 0}},
    {{-0.5f,-0.5f,-0.5f}, {1,1,1,1}, {1,1}, { 0, 0,-1}, {-1, 0, 0}},
    {{-0.5f, 0.5f,-0.5f}, {1,1,1,1}, {1,0}, { 0, 0,-1}, {-1, 0, 0}},
    {{ 0.5f, 0.5f,-0.5f}, {1,1,1,1}, {0,0}, { 0, 0,-1}, {-1, 0, 0}},
    // Right face (X+)
    {{ 0.5f,-0.5f, 0.5f}, {1,1,1,1}, {0,1}, { 1, 0, 0}, { 0, 0,-1}},
    {{ 0.5f,-0.5f,-0.5f}, {1,1,1,1}, {1,1}, { 1, 0, 0}, { 0, 0,-1}},
    {{ 0.5f, 0.5f,-0.5f}, {1,1,1,1}, {1,0}, { 1, 0, 0}, { 0, 0,-1}},
    {{ 0.5f, 0.5f, 0.5f}, {1,1,1,1}, {0,0}, { 1, 0, 0}, { 0, 0,-1}},
    // Left face (X-)
    {{-0.5f,-0.5f,-0.5f}, {1,1,1,1}, {0,1}, {-1, 0, 0}, { 0, 0, 1}},
    {{-0.5f,-0.5f, 0.5f}, {1,1,1,1}, {1,1}, {-1, 0, 0}, { 0, 0, 1}},
    {{-0.5f, 0.5f, 0.5f}, {1,1,1,1}, {1,0}, {-1, 0, 0}, { 0, 0, 1}},
    {{-0.5f, 0.5f,-0.5f}, {1,1,1,1}, {0,0}, {-1, 0, 0}, { 0, 0, 1}},
    // Top face (Y+)
    {{-0.5f, 0.5f, 0.5f}, {1,1,1,1}, {0,1}, { 0, 1, 0}, { 1, 0, 0}},
    {{ 0.5f, 0.5f, 0.5f}, {1,1,1,1}, {1,1}, { 0, 1, 0}, { 1, 0, 0}},
    {{ 0.5f, 0.5f,-0.5f}, {1,1,1,1}, {1,0}, { 0, 1, 0}, { 1, 0, 0}},
    {{-0.5f, 0.5f,-0.5f}, {1,1,1,1}, {0,0}, { 0, 1, 0}, { 1, 0, 0}},
    // Bottom face (Y-)
    {{-0.5f,-0.5f,-0.5f}, {1,1,1,1}, {0,1}, { 0,-1, 0}, { 1, 0, 0}},
    {{ 0.5f,-0.5f,-0.5f}, {1,1,1,1}, {1,1}, { 0,-1, 0}, { 1, 0, 0}},
    {{ 0.5f,-0.5f, 0.5f}, {1,1,1,1}, {1,0}, { 0,-1, 0}, { 1, 0, 0}},
    {{-0.5f,-0.5f, 0.5f}, {1,1,1,1}, {0,0}, { 0,-1, 0}, { 1, 0, 0}},
};

static const uint16_t g_cubeIndices[] = {
     0, 1, 2,  2, 3, 0,   // front
     4, 5, 6,  6, 7, 4,   // back
     8, 9,10, 10,11, 8,   // right
    12,13,14, 14,15,12,   // left
    16,17,18, 18,19,16,   // top
    20,21,22, 22,23,20,   // bottom
};

// ============================================================================
// Grid geometry
// ============================================================================

static std::vector<GridVertex> BuildGridVertices()
{
    std::vector<GridVertex> verts;
    const int N = 10;
    const float S = 1.0f;
    const float Y = 0.0f;
    for (int i = -N; i <= N; i++) {
        float f = i * S;
        verts.push_back({{f, Y, -N * S}});
        verts.push_back({{f, Y,  N * S}});
        verts.push_back({{-N * S, Y, f}});
        verts.push_back({{ N * S, Y, f}});
    }
    return verts;
}

// ============================================================================
// GL shader compilation helpers (verbatim from cube_handle_gl_macos)
// ============================================================================

static GLuint CompileShader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOG_ERROR("Shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint LinkProgram(GLuint vs, GLuint fs)
{
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        LOG_ERROR("Program link error: %s", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// ============================================================================
// OpenGL renderer
// ============================================================================

struct GLRenderer {
    // Shader programs
    GLuint cubeProgram;
    GLuint gridProgram;
    GLuint fadeProgram;   //!< zone content-alpha edge fade
    GLuint blitProgram;   //!< texture-mode IOSurface -> default-framebuffer present

    // Cube uniforms
    GLint cubeLocMVP, cubeLocModel, cubeLocTexSize;
    GLint cubeLocBasecolor, cubeLocNormal, cubeLocAO;

    // Grid uniforms
    GLint gridLocMVP, gridLocColor;

    // Fade uniforms
    GLint fadeLocTilePx, fadeLocFeatherPx;
    GLuint fadeVAO;       //!< attribute-less; bound for the fullscreen-triangle draw

    // Blit/present uniforms
    GLint blitLocTex, blitLocTexSize;
    GLuint blitVAO;       //!< attribute-less; bound for the fullscreen-triangle draw

    // Geometry
    GLuint cubeVAO, cubeVBO, cubeEBO;
    GLuint gridVAO, gridVBO;
    int gridVertexCount;

    // Textures (GL_TEXTURE_RECTANGLE for loaded images)
    GLuint textures[3]; // basecolor, normal, AO
    int texSizes[3][2]; // width, height per texture

    // Depth buffer (renderbuffer) for the (fallback) projection FBO
    GLuint depthRBO;
    GLuint fbo;
    uint32_t depthWidth, depthHeight;

    float cubeRotation;
};

// ============================================================================
// Texture loading (into GL_TEXTURE_RECTANGLE; verbatim from cube_handle_gl_macos)
// ============================================================================

static GLuint LoadTextureRect(const char *path, int *outW, int *outH,
                               uint8_t fallbackR, uint8_t fallbackG, uint8_t fallbackB)
{
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_RECTANGLE, tex);

    int w, h, channels;
    stbi_uc *pixels = stbi_load(path, &w, &h, &channels, 4);

    if (!pixels) {
        LOG_WARN("Texture not found: %s (using fallback)", path);
        w = h = 1;
        uint8_t fallback[4] = {fallbackR, fallbackG, fallbackB, 255};
        glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA8, 1, 1, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, fallback);
    } else {
        glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA8, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        stbi_image_free(pixels);
        LOG_INFO("Loaded texture: %s (%dx%d)", path, w, h);
    }

    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_RECTANGLE, 0);

    *outW = w;
    *outH = h;
    return tex;
}

// ============================================================================
// Renderer setup
// ============================================================================

static bool InitRenderer(GLRenderer &r)
{
    LOG_INFO("OpenGL context:");
    LOG_INFO("  GL_VERSION: %s", glGetString(GL_VERSION));
    LOG_INFO("  GL_RENDERER: %s", glGetString(GL_RENDERER));
    LOG_INFO("  GL_VENDOR: %s", glGetString(GL_VENDOR));

    // Compile cube shaders
    GLuint cubeVS = CompileShader(GL_VERTEX_SHADER, g_cubeVertexShader);
    GLuint cubeFS = CompileShader(GL_FRAGMENT_SHADER, g_cubeFragmentShader);
    if (!cubeVS || !cubeFS) return false;
    r.cubeProgram = LinkProgram(cubeVS, cubeFS);
    glDeleteShader(cubeVS);
    glDeleteShader(cubeFS);
    if (!r.cubeProgram) return false;

    r.cubeLocMVP = glGetUniformLocation(r.cubeProgram, "uMVP");
    r.cubeLocModel = glGetUniformLocation(r.cubeProgram, "uModel");
    r.cubeLocTexSize = glGetUniformLocation(r.cubeProgram, "uTexSize");
    r.cubeLocBasecolor = glGetUniformLocation(r.cubeProgram, "uBasecolorTex");
    r.cubeLocNormal = glGetUniformLocation(r.cubeProgram, "uNormalTex");
    r.cubeLocAO = glGetUniformLocation(r.cubeProgram, "uAOTex");

    // Compile grid shaders
    GLuint gridVS = CompileShader(GL_VERTEX_SHADER, g_gridVertexShader);
    GLuint gridFS = CompileShader(GL_FRAGMENT_SHADER, g_gridFragmentShader);
    if (!gridVS || !gridFS) return false;
    r.gridProgram = LinkProgram(gridVS, gridFS);
    glDeleteShader(gridVS);
    glDeleteShader(gridFS);
    if (!r.gridProgram) return false;

    r.gridLocMVP = glGetUniformLocation(r.gridProgram, "uMVP");
    r.gridLocColor = glGetUniformLocation(r.gridProgram, "uColor");

    // Compile zone edge-fade shaders
    GLuint fadeVS = CompileShader(GL_VERTEX_SHADER, g_fadeVertexShader);
    GLuint fadeFS = CompileShader(GL_FRAGMENT_SHADER, g_fadeFragmentShader);
    if (!fadeVS || !fadeFS) return false;
    r.fadeProgram = LinkProgram(fadeVS, fadeFS);
    glDeleteShader(fadeVS);
    glDeleteShader(fadeFS);
    if (!r.fadeProgram) return false;
    r.fadeLocTilePx = glGetUniformLocation(r.fadeProgram, "uTilePx");
    r.fadeLocFeatherPx = glGetUniformLocation(r.fadeProgram, "uFeatherPx");
    glGenVertexArrays(1, &r.fadeVAO);

    // Compile blit/present shaders
    GLuint blitVS = CompileShader(GL_VERTEX_SHADER, g_blitVertexShader);
    GLuint blitFS = CompileShader(GL_FRAGMENT_SHADER, g_blitFragmentShader);
    if (!blitVS || !blitFS) return false;
    r.blitProgram = LinkProgram(blitVS, blitFS);
    glDeleteShader(blitVS);
    glDeleteShader(blitFS);
    if (!r.blitProgram) return false;
    r.blitLocTex = glGetUniformLocation(r.blitProgram, "uTex");
    r.blitLocTexSize = glGetUniformLocation(r.blitProgram, "uTexSize");
    glGenVertexArrays(1, &r.blitVAO);

    // Cube VAO
    glGenVertexArrays(1, &r.cubeVAO);
    glBindVertexArray(r.cubeVAO);

    glGenBuffers(1, &r.cubeVBO);
    glBindBuffer(GL_ARRAY_BUFFER, r.cubeVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(g_cubeVertices), g_cubeVertices, GL_STATIC_DRAW);

    glGenBuffers(1, &r.cubeEBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r.cubeEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(g_cubeIndices), g_cubeIndices, GL_STATIC_DRAW);

    // pos
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(CubeVertex),
                          (void *)offsetof(CubeVertex, pos));
    glEnableVertexAttribArray(0);
    // color
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(CubeVertex),
                          (void *)offsetof(CubeVertex, color));
    glEnableVertexAttribArray(1);
    // uv
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(CubeVertex),
                          (void *)offsetof(CubeVertex, uv));
    glEnableVertexAttribArray(2);
    // normal
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(CubeVertex),
                          (void *)offsetof(CubeVertex, normal));
    glEnableVertexAttribArray(3);
    // tangent
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(CubeVertex),
                          (void *)offsetof(CubeVertex, tangent));
    glEnableVertexAttribArray(4);

    glBindVertexArray(0);

    // Grid VAO
    auto gridVerts = BuildGridVertices();
    r.gridVertexCount = (int)gridVerts.size();

    glGenVertexArrays(1, &r.gridVAO);
    glBindVertexArray(r.gridVAO);

    glGenBuffers(1, &r.gridVBO);
    glBindBuffer(GL_ARRAY_BUFFER, r.gridVBO);
    glBufferData(GL_ARRAY_BUFFER, gridVerts.size() * sizeof(GridVertex), gridVerts.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GridVertex), (void *)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);

    // Load textures (GL_TEXTURE_RECTANGLE)
    std::string texDir = GetTextureDir();
    r.textures[0] = LoadTextureRect((texDir + "Wood_Crate_001_basecolor.jpg").c_str(),
                                     &r.texSizes[0][0], &r.texSizes[0][1], 200, 200, 200);
    r.textures[1] = LoadTextureRect((texDir + "Wood_Crate_001_normal.jpg").c_str(),
                                     &r.texSizes[1][0], &r.texSizes[1][1], 128, 128, 255);
    r.textures[2] = LoadTextureRect((texDir + "Wood_Crate_001_ambientOcclusion.jpg").c_str(),
                                     &r.texSizes[2][0], &r.texSizes[2][1], 255, 255, 255);

    r.cubeRotation = 0.0f;
    r.depthRBO = 0;
    r.fbo = 0;
    r.depthWidth = r.depthHeight = 0;

    LOG_INFO("OpenGL renderer initialized");
    return true;
}

// ============================================================================
// Ensure FBO + depth renderbuffer for rendering into swapchain textures
// (verbatim from cube_handle_gl_macos)
// ============================================================================

static void EnsureFBO(GLRenderer &r, uint32_t w, uint32_t h)
{
    if (r.fbo == 0) {
        glGenFramebuffers(1, &r.fbo);
    }
    if (r.depthRBO == 0 || r.depthWidth != w || r.depthHeight != h) {
        if (r.depthRBO) glDeleteRenderbuffers(1, &r.depthRBO);
        glGenRenderbuffers(1, &r.depthRBO);
        glBindRenderbuffer(GL_RENDERBUFFER, r.depthRBO);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, w, h);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        r.depthWidth = w;
        r.depthHeight = h;
    }
}

// Auto-detect whether a swapchain texture name is bound to GL_TEXTURE_2D
// (GL native compositor) or GL_TEXTURE_RECTANGLE (Metal/IOSurface-backed).
// Mirrors the probe in cube_handle_gl_macos::RenderScene.
static GLenum DetectSwapchainTexTarget(GLuint tex)
{
    GLint prev2d = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2d);
    glBindTexture(GL_TEXTURE_2D, tex);
    GLint w2d = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w2d);
    glBindTexture(GL_TEXTURE_2D, prev2d);
    return (w2d > 0) ? GL_TEXTURE_2D : GL_TEXTURE_RECTANGLE;
}

// Upload a CPU BGRA byte buffer (B,G,R,A order in memory) into a swapchain
// texture name (TEXTURE_2D or RECTANGLE auto-detected). GL_BGRA +
// GL_UNSIGNED_INT_8_8_8_8_REV matches the BGRA byte layout on little-endian.
// rowLengthPx is the source row length in PIXELS (0 = tightly packed = w).
static void UploadBGRAToSwapchainTex(GLuint tex, uint32_t w, uint32_t h,
                                     const void *bgra, uint32_t rowLengthPx)
{
    GLenum target = DetectSwapchainTexTarget(tex);
    glBindTexture(target, tex);
    if (rowLengthPx != 0) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)rowLengthPx);
    }
    glTexSubImage2D(target, 0, 0, 0, (GLsizei)w, (GLsizei)h,
                    GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, bgra);
    if (rowLengthPx != 0) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    glBindTexture(target, 0);
}

// ============================================================================
// Render scene into swapchain image
// ============================================================================

static void RenderScene(GLRenderer &r, GLuint targetTex, uint32_t targetW, uint32_t targetH,
                         const EyeRenderParams *eyes, int eyeCount)
{
    EnsureFBO(r, targetW, targetH);

    glBindFramebuffer(GL_FRAMEBUFFER, r.fbo);
    GLenum texTarget = DetectSwapchainTexTarget(targetTex);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texTarget, targetTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, r.depthRBO);

    GLenum fbStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("Framebuffer incomplete: 0x%x", fbStatus);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    // Transparent-background mode (DISPLAYXR_TRANSPARENT_BG=1) clears RGBA(0,0,0,0)
    // so the desktop shows through everywhere the cube isn't drawn. Pairs with
    // XrCocoaWindowBindingCreateInfoEXT.transparentBackgroundEnabled = XR_TRUE.
    static const bool transparent_bg = []() {
        const char *e = getenv("DISPLAYXR_TRANSPARENT_BG");
        return e != nullptr && *e != '\0' && *e != '0';
    }();
    if (transparent_bg) {
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    } else {
        glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    for (int e = 0; e < eyeCount; e++) {
        const EyeRenderParams &eye = eyes[e];
        glViewport(eye.viewportX, eye.viewportY, eye.width, eye.height);
        glScissor(eye.viewportX, eye.viewportY, eye.width, eye.height);
        glEnable(GL_SCISSOR_TEST);

        float vp_mat[16];
        mat4_multiply(vp_mat, eye.projMat, eye.viewMat);

        // --- Draw cube ---
        {
            const float cubeSize = 0.06f;
            const float cubeHeight = cubeSize / 2.0f;
            float model[16], rotation[16], translation[16], scale[16], tmp[16];
            mat4_scaling(scale, cubeSize);
            mat4_rotation_y(rotation, r.cubeRotation);
            mat4_translation(translation, 0.0f, cubeHeight, 0.0f);
            mat4_multiply(tmp, scale, rotation);
            mat4_multiply(model, translation, tmp);

            float mvp[16];
            mat4_multiply(mvp, vp_mat, model);

            glUseProgram(r.cubeProgram);
            glUniformMatrix4fv(r.cubeLocMVP, 1, GL_FALSE, mvp);
            glUniformMatrix4fv(r.cubeLocModel, 1, GL_FALSE, model);
            glUniform2f(r.cubeLocTexSize,
                        (float)r.texSizes[0][0], (float)r.texSizes[0][1]);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_RECTANGLE, r.textures[0]);
            glUniform1i(r.cubeLocBasecolor, 0);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_RECTANGLE, r.textures[1]);
            glUniform1i(r.cubeLocNormal, 1);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_RECTANGLE, r.textures[2]);
            glUniform1i(r.cubeLocAO, 2);

            glBindVertexArray(r.cubeVAO);
            glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
        }

        // --- Draw grid ---
        {
            const float gridScale = 0.05f;
            float gridScl[16], gridMvp[16];
            mat4_scaling(gridScl, gridScale);
            mat4_multiply(gridMvp, vp_mat, gridScl);

            glUseProgram(r.gridProgram);
            glUniformMatrix4fv(r.gridLocMVP, 1, GL_FALSE, gridMvp);
            glUniform4f(r.gridLocColor, 0.3f, 0.3f, 0.35f, 1.0f);

            glBindVertexArray(r.gridVAO);
            glDrawArrays(GL_LINES, 0, r.gridVertexCount);
        }
    }

    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(0);
    glUseProgram(0);
    glFlush();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ============================================================================
// Texture-mode shared IOSurface (lifted from cube_texture_metal_macos)
// ============================================================================
//
// The app provides a shared IOSurface as the runtime's render/composite
// target (chained as XrCocoaWindowBindingCreateInfoEXT.sharedIOSurface). For
// the zones path the runtime composites the full-window multi-zone super-atlas
// DIRECTLY into this surface (the declared output rect is superseded by zones),
// so the app just presents the whole surface region. No 2D surround needed.

static IOSurfaceRef g_ioSurface = NULL;
static uint32_t g_ioSurfaceWidth = 1920;
static uint32_t g_ioSurfaceHeight = 1080;
static GLuint g_presentTex = 0;  //!< lazily-created GL_TEXTURE_RECTANGLE over g_ioSurface

// Autonomous-verification dump path (DXR_TEXDUMP). "1" or empty-but-present →
// default /tmp/zones_texture_readback.png; any other value is the literal path;
// unset → no auto-dump (the /tmp/dxr_atlas_trigger file-trigger still works).
static std::string g_texDumpPath;
static bool g_texDumpEnabled = false;
static bool g_texDumpDone = false;          //!< one-shot at the warmup frame gate
static const long kTexDumpFrame = 150;      //!< matches the macOS capture convention

// Framebuffer capture (preferred over the CPU IOSurface readback): a CPU
// IOSurfaceLock of a surface the runtime wrote via GL can race the GPU (the GL
// compositor only glFlush()es, not glFinish()), so the lock often reads zeros
// even when the on-screen present is correct. Instead we glReadPixels the app's
// BACK buffer right after the present blit — GL-synchronized in our own context
// and exactly "what's on screen". Set by the trigger, consumed inside the blit.
static bool g_fbCaptureReq = false;
static std::string g_fbCapturePath;

// ============================================================================
// Globals
// ============================================================================

static volatile bool g_running = true;
static NSWindow *g_window = nil;
static NSOpenGLView *g_glView = nil;
static NSOpenGLContext *g_glContext = nil;

// Input state (mirrors cube_handle_vk_macos)
struct InputState {
    float yaw = 0.0f, pitch = 0.0f;
    bool keyW = false, keyA = false, keyS = false, keyD = false;
    bool keyE = false, keyQ = false;
    float cameraPosX = 0.0f, cameraPosY = 0.0f, cameraPosZ = 0.0f;
    bool resetViewRequested = false;
    ViewParams viewParams;
    bool hudVisible = true;
    // Rendering mode REQUESTS — single source of truth lives on the runtime
    // side (read back as app.currentModeIndex after the runtime's
    // XrEventDataRenderingModeChangedEXT lands). Keys emit transient requests;
    // the actual current mode is never mirrored here.
    uint32_t renderingModeCount = 0;             // mirror of app.renderingModeCount for keypress bounds
    bool cycleRenderingModeRequested = false;    // V key
    int32_t absoluteRenderingModeRequested = -1; // 0-8 keys; -1 = none
    bool cameraMode = false;
    float nominalViewerZ = 0.5f;
    // Disturbance-free rig round-trip (C) + absolute reset (SPACE) — delegated
    // to the shared displayxr-common helper (common/rig_mode.{h,cpp}). The C key
    // sets the request; the conversion runs in UpdateCameraMovement using the
    // CANVAS size + initial vHeight fed in below.
    bool rigModeToggleRequested = false;
    float canvasWidthM = 0.0f;
    float canvasHeightM = 0.0f;
    float initialVirtualDisplayHeight = 0.0f;
    // 'I' key: snapshot the **app's projection-layer atlas only** (cols ×
    // rows × renderW × renderH) to ~/Pictures/DisplayXR/<app>-<N>_
    // <cols>x<rows>.png. One projection layer; HUD / window-space layers
    // and per-eye disparity are NOT included. Skipped for 1×1. For the
    // runtime's full post-compose atlas use the trigger file
    // $TMPDIR/displayxr_atlas_trigger (see issue #210).
    bool captureAtlasRequested = false;
};
static InputState g_input;
static const float CAMERA_HALF_TAN_VFOV = 0.32491969623f; // tan(18deg) -> 36deg vFOV

// HUD window-space layer (XR_EXT_window_space_layer): a fractional-window-space
// quad with per-eye disparity. Replaces the legacy NSView overlay so the runtime
// composes the HUD through the proper extension path on macOS, matching Windows.
static const uint32_t HUD_PIXEL_WIDTH = 380;
static const uint32_t HUD_PIXEL_HEIGHT = 470;
static const float HUD_WIDTH_FRACTION = 0.20f;

// Performance stats
static double g_avgFrameTime = 0.0;
static float g_hudUpdateTimer = 0.0f;
static uint32_t g_windowW = 1512, g_windowH = 823;

// ============================================================================
// #439 Phase 3 — handle + mask + Local2D layer modes (§8 cases 2/3/4)
// ============================================================================
// DXR_LOCAL2D_PANEL=1  — submit a Local2D panel layer (case 3: layer-only,
//                        IMPLICIT mask from the panel rect, zero mask calls).
// DXR_LOCAL2D_MASK=1   — additionally create + submit an explicit Tier-2
//                        mask with 3D island rects (case 2: the first
//                        handle + mask + layer app — islands weave, panel
//                        crisp, desktop visible where neither covers).
// DXR_LOCAL2D_PANEL2=1 — additionally submit a second, overlapping panel
//                        with XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT
//                        (case 4: list-order stacking + alpha fringing).
static bool g_l2dPanel = false;
static bool g_l2dMask = false;
static bool g_l2dPanel2 = false;
static bool g_l2dActive = false;   // set once panels (+ optional mask) are live
static long g_frameCounter = 0;
static const int g_l2dActivationFrame = 10;
static uint32_t g_renderW = 0, g_renderH = 0;

// ============================================================================
// XR_EXT_display_zones state (ADR-027)
// ============================================================================

static const uint32_t kNumZones = 2;

// Per-zone rig framing for the test: virtual display height in app units
// (shared by both zones; the cube is 0.06 m tall).
static const float kZoneVirtualDisplayHeight = 0.30f;

struct DisplayZone {
    uint32_t zoneId = 0;
    XrRect2Di rect = {};            //!< client-window (backing) pixels; locate AND submit use this one variable
    float ipdFactor = 1.0f;
    float perspectiveFactor = 1.0f;
    float spinPhase = 0.0f;         //!< added to the shared cube rotation for this zone
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f}; //!< premultiplied RGBA — zone blends are
                                                    //!< expressed through content alpha (ADR-027)
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t tileW = 0;             //!< per-view tile width (= recommended view width)
    uint32_t tileH = 0;             //!< per-view tile height
    uint32_t tileCount = 0;         //!< view tiles in the horizontally tiled swapchain
    std::vector<GLuint> images;
    GLuint fbo = 0;                 //!< per-zone FBO (color = the zone's swapchain image)
    GLuint depthRBO = 0;            //!< per-zone depth (sizes differ between zones)
    uint32_t depthW = 0, depthH = 0;
};
static DisplayZone g_zonesArr[kNumZones];

// Rects are resolved from the live window backing size at activation, using
// the same proportions as the D3D11 source app's fixed 1280x720 window:
// strip = top 25%, zone A = left half below the strip, zone B home / overlap
// per the {700,180,520,360} / {400,300,520,360} fractions.
static XrRect2Di g_zoneARect, g_zoneBRect, g_zoneBOverlapRect, g_stripRect;
static bool g_zoneBOverlap = false;

// Local2D strip (always on in zones mode; filled once via CPU upload).
struct StripLayer {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t w = 0, h = 0;
};
static StripLayer g_strip;

// Zones activation: created a few frames in, once the session runs.
static bool g_zonesActive = false;
static bool g_zonesAttempted = false;
static const long kZonesActivationFrame = 10;

// Wish modes (M key): 0 AUTO, 1 explicit Tier-2 rects. Tier-3 (freeform
// render-target mask) has no Metal binding struct — not reachable here.
// DXR_ZONES_WISH_MODE / DXR_ZONES_OVERLAP preselect the M / O key states
// for headless validation (macOS key injection needs TCC permissions).
static int g_wishMode = 0;

// Edge-triggered key requests (set in PumpMacOSEvents, consumed in the loop).
static bool g_wishModeCycleRequested = false;
static bool g_overlapToggleRequested = false;

// XR_EXT_display_zones harness.
static bool g_hasDisplayZonesExt = false;
static PFN_xrGetDisplayZoneCapabilitiesEXT g_pfnGetZoneCaps = nullptr;
static PFN_xrGetDisplayZoneRecommendedViewSizeEXT g_pfnGetZoneViewSize = nullptr;

// DXR_ZONES_VALIDATE=1 — chain the validate bit on every frame-end info.
static bool ZonesValidateEnabled() {
    static const bool e = []() {
        const char* v = getenv("DXR_ZONES_VALIDATE");
        return v != nullptr && *v == '1';
    }();
    return e;
}

// Cached HUD section strings, refreshed at HUD throttle rate (~2 Hz).
static std::wstring g_hudSessionText, g_hudModeText, g_hudPerfText;
static std::wstring g_hudDisplayText, g_hudEyeText, g_hudCameraText;
static std::wstring g_hudStereoText, g_hudHelpText;

static void SignalHandler(int)
{
    g_running = false;
}

// ============================================================================
// macOS window creation (NSOpenGLView-backed, GL 4.1 Core)
// ============================================================================

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
    return NSTerminateCancel;
}
@end

@interface AppWindowDelegate : NSObject <NSWindowDelegate>
@end
@implementation AppWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;
    g_running = false;
    return NO;
}
@end

static AppDelegate *g_appDelegate = nil;
static AppWindowDelegate *g_windowDelegate = nil;

static bool CreateMacOSWindow(uint32_t width, uint32_t height)
{
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

        [g_window setTitle:@"OpenGL Cube Zones TEXTURE — XR_EXT_display_zones parity"];
        [g_window setAcceptsMouseMovedEvents:YES];
        [g_window setReleasedWhenClosed:NO];

        g_windowDelegate = [[AppWindowDelegate alloc] init];
        [g_window setDelegate:g_windowDelegate];

        // Create NSOpenGLView with a core 4.1 profile context
        NSOpenGLPixelFormatAttribute attrs[] = {
            NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
            NSOpenGLPFAColorSize, 24,
            NSOpenGLPFAAlphaSize, 8,
            NSOpenGLPFADepthSize, 24,
            NSOpenGLPFADoubleBuffer,
            NSOpenGLPFAAccelerated,
            0
        };
        NSOpenGLPixelFormat *pixelFormat = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
        if (!pixelFormat) {
            LOG_ERROR("Failed to create NSOpenGLPixelFormat");
            return false;
        }

        g_glView = [[NSOpenGLView alloc] initWithFrame:frame pixelFormat:pixelFormat];
        if (!g_glView) {
            LOG_ERROR("Failed to create NSOpenGLView");
            return false;
        }

        [g_glView setWantsBestResolutionOpenGLSurface:YES];
        g_glContext = [g_glView openGLContext];
        [g_glContext makeCurrentContext];

        [g_window setContentView:g_glView];
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

    if (g_window == nil || g_glView == nil || g_glContext == nil) {
        LOG_ERROR("Failed to create macOS window");
        return false;
    }

    LOG_INFO("Created macOS window (%ux%u) with NSOpenGLView (GL 4.1 Core)", width, height);
    return true;
}

// ============================================================================
// Texture-mode IOSurface: create, present (blit to drawable), readback dump
// ============================================================================

static bool CreateIOSurface(uint32_t width, uint32_t height)
{
    NSDictionary *props = @{
        (id)kIOSurfaceWidth:       @(width),
        (id)kIOSurfaceHeight:      @(height),
        (id)kIOSurfaceBytesPerElement: @(4),
        (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA'),
    };

    g_ioSurface = IOSurfaceCreate((CFDictionaryRef)props);
    if (g_ioSurface == NULL) {
        LOG_ERROR("Failed to create IOSurface (%ux%u)", width, height);
        return false;
    }

    g_ioSurfaceWidth = width;
    g_ioSurfaceHeight = height;

    LOG_INFO("Created shared IOSurface: %ux%u, BGRA8, id=%u", width, height,
             IOSurfaceGetID(g_ioSurface));
    return true;
}

// Present: map the runtime-composited IOSurface as a GL_TEXTURE_RECTANGLE in
// the app's own on-screen context and blit it to the default framebuffer. For
// the zones path the whole window region of the surface carries the multi-zone
// composite (output rect superseded by zones), so we present it 1:1.
static void BlitIOSurfaceToDefaultFramebuffer(GLRenderer &r)
{
    if (g_ioSurface == NULL || g_glContext == nil) return;

    [g_glContext makeCurrentContext];

    if (g_presentTex == 0) {
        glGenTextures(1, &g_presentTex);
    }

    glBindTexture(GL_TEXTURE_RECTANGLE, g_presentTex);
    CGLError cglErr = CGLTexImageIOSurface2D(
        CGLGetCurrentContext(), GL_TEXTURE_RECTANGLE, GL_RGBA8,
        (GLsizei)g_ioSurfaceWidth, (GLsizei)g_ioSurfaceHeight,
        GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, g_ioSurface, 0);
    if (cglErr != kCGLNoError) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_WARN("CGLTexImageIOSurface2D failed (0x%x)", (unsigned)cglErr);
        }
    }
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Default framebuffer, viewport in backing pixels.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    GLsizei vpW = (GLsizei)g_windowW;
    GLsizei vpH = (GLsizei)g_windowH;
    if (g_glView != nil) {
        NSRect backing = [g_glView convertRectToBacking:[g_glView bounds]];
        if (backing.size.width > 0 && backing.size.height > 0) {
            vpW = (GLsizei)backing.size.width;
            vpH = (GLsizei)backing.size.height;
        }
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, vpW, vpH);
    glClearColor(0.08f, 0.08f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(r.blitProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_RECTANGLE, g_presentTex);
    glUniform1i(r.blitLocTex, 0);
    glUniform2f(r.blitLocTexSize, (float)g_ioSurfaceWidth, (float)g_ioSurfaceHeight);

    glBindVertexArray(r.blitVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_RECTANGLE, 0);

    // GPU-synchronized capture of the presented frame (see g_fbCaptureReq). Read
    // GL_BACK before flushBuffer; glReadPixels finishes the prior draw so this is
    // exactly the composited IOSurface as sampled on screen (alpha included).
    if (g_fbCaptureReq) {
        g_fbCaptureReq = false;
        glReadBuffer(GL_BACK);
        std::vector<uint8_t> px((size_t)vpW * vpH * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, vpW, vpH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        // glReadPixels is bottom-up; stb wants top-down — flip rows.
        std::vector<uint8_t> flipped((size_t)vpW * vpH * 4);
        for (GLsizei y = 0; y < vpH; y++) {
            memcpy(flipped.data() + (size_t)y * vpW * 4,
                   px.data() + (size_t)(vpH - 1 - y) * vpW * 4,
                   (size_t)vpW * 4);
        }
        int ok = stbi_write_png(g_fbCapturePath.c_str(), (int)vpW, (int)vpH, 4,
                                flipped.data(), (int)(vpW * 4));
        LOG_WARN("FRAMEBUFFER CAPTURE %s: %s (%dx%d)",
                 ok ? "DUMPED" : "FAILED", g_fbCapturePath.c_str(), (int)vpW, (int)vpH);
    }

    [g_glContext flushBuffer];
}

// Autonomous verification: read the shared IOSurface back to a PNG. This is
// the key proof — it captures exactly what the TEXTURE APP received in its
// shared surface (the full multi-zone composite the runtime wrote back). The
// IOSurface is BGRA8; stbi_write_png wants RGBA, so swizzle B<->R on copy.
static void DumpIOSurfaceToPNG(const char *path)
{
    if (g_ioSurface == NULL || path == NULL || path[0] == '\0') return;

    IOSurfaceLock(g_ioSurface, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *base = (const uint8_t *)IOSurfaceGetBaseAddress(g_ioSurface);
    size_t srcStride = IOSurfaceGetBytesPerRow(g_ioSurface);
    uint32_t w = g_ioSurfaceWidth;
    uint32_t h = g_ioSurfaceHeight;
    if (base == NULL || w == 0 || h == 0) {
        IOSurfaceUnlock(g_ioSurface, kIOSurfaceLockReadOnly, NULL);
        LOG_WARN("TEXTURE READBACK: IOSurface base/dims unavailable — skipped");
        return;
    }

    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *srow = base + (size_t)y * srcStride;
        uint8_t *drow = rgba.data() + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *sp = srow + (size_t)x * 4; // B,G,R,A
            uint8_t *dp = drow + (size_t)x * 4;        // R,G,B,A
            dp[0] = sp[2];
            dp[1] = sp[1];
            dp[2] = sp[0];
            dp[3] = sp[3];
        }
    }
    IOSurfaceUnlock(g_ioSurface, kIOSurfaceLockReadOnly, NULL);

    int ok = stbi_write_png(path, (int)w, (int)h, 4, rgba.data(), (int)(w * 4));
    if (ok) {
        LOG_WARN("TEXTURE READBACK DUMPED: %s", path);
    } else {
        LOG_WARN("TEXTURE READBACK FAILED to write: %s", path);
    }
}

// Per-frame texture-mode tail: present the IOSurface the runtime composited
// into (app owns presentation) and run autonomous verification. Two dump
// paths, both reading the IOSurface back AFTER the present so they capture
// exactly what the texture app received:
//   - DXR_TEXDUMP: one-shot at the warmup frame gate (~frame 150).
//   - /tmp/dxr_atlas_trigger: the macOS file-trigger convention (touch it to
//     dump on demand to /tmp/dxr_atlas.png).
static void PresentAndMaybeDump(GLRenderer &renderer)
{
    // Arm captures BEFORE the blit — the framebuffer grab happens inside it.
    // NB: a SEPARATE trigger from the runtime's /tmp/dxr_atlas_trigger (which the
    // GL compositor consumes to dump its pre-DP atlas to /tmp/dxr_atlas.png), so
    // the two captures don't race for the same file.
    bool triggerDump = (access("/tmp/gl_app_trigger", F_OK) == 0);
    if (triggerDump) {
        unlink("/tmp/gl_app_trigger");
        g_fbCaptureReq = true;
        g_fbCapturePath = "/tmp/gl_app_present.png";
    }
    bool warmupDump =
        (g_texDumpEnabled && !g_texDumpDone && g_frameCounter >= kTexDumpFrame);
    if (warmupDump && !triggerDump) {
        g_fbCaptureReq = true;
        g_fbCapturePath = g_texDumpPath;
    }

    BlitIOSurfaceToDefaultFramebuffer(renderer);

    // Secondary CPU IOSurface readback (diagnostic; may race the GPU — see note
    // on g_fbCaptureReq). Kept so the two paths can be compared.
    if (warmupDump) {
        g_texDumpDone = true;
        DumpIOSurfaceToPNG(g_texDumpPath.c_str());
    }
    if (triggerDump) {
        DumpIOSurfaceToPNG("/tmp/gl_app_iosurface.png");
    }
}

// ============================================================================
// macOS event pump (input handling)
// ============================================================================

static void PumpMacOSEvents()
{
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
                    g_input.viewParams.ipdFactor *= factor;
                    if (g_input.viewParams.ipdFactor < 0.0f) g_input.viewParams.ipdFactor = 0.0f;
                    if (g_input.viewParams.ipdFactor > 1.0f) g_input.viewParams.ipdFactor = 1.0f;
                } else if (scrollMods & NSEventModifierFlagControl) {
                    g_input.viewParams.parallaxFactor *= factor;
                    if (g_input.viewParams.parallaxFactor < 0.0f) g_input.viewParams.parallaxFactor = 0.0f;
                    if (g_input.viewParams.parallaxFactor > 1.0f) g_input.viewParams.parallaxFactor = 1.0f;
                } else if (scrollMods & NSEventModifierFlagOption) {
                    if (g_input.cameraMode) {
                        g_input.viewParams.invConvergenceDistance *= factor;
                        if (g_input.viewParams.invConvergenceDistance < 0.1f) g_input.viewParams.invConvergenceDistance = 0.1f;
                        if (g_input.viewParams.invConvergenceDistance > 10.0f) g_input.viewParams.invConvergenceDistance = 10.0f;
                    } else {
                        g_input.viewParams.perspectiveFactor *= factor;
                        if (g_input.viewParams.perspectiveFactor < 0.1f) g_input.viewParams.perspectiveFactor = 0.1f;
                        if (g_input.viewParams.perspectiveFactor > 10.0f) g_input.viewParams.perspectiveFactor = 10.0f;
                    }
                } else {
                    if (g_input.cameraMode) {
                        g_input.viewParams.zoomFactor *= factor;
                        if (g_input.viewParams.zoomFactor < 0.1f) g_input.viewParams.zoomFactor = 0.1f;
                        if (g_input.viewParams.zoomFactor > 10.0f) g_input.viewParams.zoomFactor = 10.0f;
                    } else {
                        g_input.viewParams.scaleFactor *= factor;
                        if (g_input.viewParams.scaleFactor < 0.1f) g_input.viewParams.scaleFactor = 0.1f;
                        if (g_input.viewParams.scaleFactor > 10.0f) g_input.viewParams.scaleFactor = 10.0f;
                    }
                }
            } else if (type == NSEventTypeKeyDown) {
                if ([[event characters] length] > 0) {
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
                    else if ([event keyCode] == 48 /* kVK_Tab */ && !isRepeat &&
                             ([event modifierFlags] & NSEventModifierFlagShift)) {
                        // SHIFT+TAB so bare TAB stays free for the workspace shell's
                        // focus-cycle binding (matches the Windows test apps).
                        // NSEvent.characters returns 0x19 (NSBackTabCharacter) for
                        // SHIFT+TAB, not '\t' — gate on the hardware keyCode.
                        g_input.hudVisible = !g_input.hudVisible;
                    }
                    else if (ch == 'v' && !isRepeat) {
                        g_input.cycleRenderingModeRequested = true;
                    }
                    else if (ch == 'm' && !isRepeat) {
                        g_wishModeCycleRequested = true;
                    }
                    else if (ch == 'o' && !isRepeat) {
                        g_overlapToggleRequested = true;
                    }
                    else if ((ch == 'i' || ch == 'I') && !isRepeat) {
                        g_input.captureAtlasRequested = true;
                    }
                    else if (ch == 'c' && !isRepeat) {
                        // Disturbance-free rig round-trip — the conversion runs
                        // in UpdateCameraMovement (it has the canvas size).
                        g_input.rigModeToggleRequested = true;
                    }
                    else if (ch >= '0' && ch <= '8' && !isRepeat) {
                        g_input.absoluteRenderingModeRequested = (int32_t)(ch - '0');
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

        // Update window pixel size (Retina-aware)
        if (g_window != nil) {
            NSSize contentSize = [[g_window contentView] bounds].size;
            CGFloat backingScale = [g_window backingScaleFactor];
            g_windowW = (uint32_t)(contentSize.width * backingScale);
            g_windowH = (uint32_t)(contentSize.height * backingScale);
        }
    }
}

// ============================================================================
// Camera movement (ported from cube_handle_vk_macos)
// ============================================================================

static void UpdateCameraMovement(InputState& state, float deltaTime, float displayHeightM = 0.0f) {
    // Absolute SPACE reset — snap back to the initial DISPLAY-centric state via
    // the shared helper (display rig, pose origin/identity, initial vHeight,
    // every tunable default incl. cameraM2v=1).
    if (state.resetViewRequested) {
        state.resetViewRequested = false;
        float pos[3] = {state.cameraPosX, state.cameraPosY, state.cameraPosZ};
        dxr::RigResetToInitial(state.viewParams, state.cameraMode, pos, state.yaw, state.pitch,
                               state.initialVirtualDisplayHeight);
        state.cameraPosX = pos[0];
        state.cameraPosY = pos[1];
        state.cameraPosZ = pos[2];
        return;
    }

    // Disturbance-free rig round-trip toggle (C key) — delegate to the shared
    // converter so macOS and Windows apps behave identically. Pass the same
    // orientation quaternion the app submits to the runtime, plus the CANVAS
    // size the runtime renders into (NOT the full display).
    if (state.rigModeToggleRequested) {
        state.rigModeToggleRequested = false;
        XrQuaternionf rq;
        quat_from_yaw_pitch(state.yaw, state.pitch, &rq);
        const float quat[4] = {rq.x, rq.y, rq.z, rq.w};
        float pos[3] = {state.cameraPosX, state.cameraPosY, state.cameraPosZ};
        dxr::RigToggleMode(state.viewParams, state.cameraMode, pos, quat, state.canvasWidthM,
                           state.canvasHeightM, state.nominalViewerZ, displayHeightM);
        state.cameraPosX = pos[0];
        state.cameraPosY = pos[1];
        state.cameraPosZ = pos[2];
        return;
    }

    float m2v = 1.0f;
    if (state.viewParams.virtualDisplayHeight > 0.0f && displayHeightM > 0.0f)
        m2v = state.viewParams.virtualDisplayHeight / displayHeightM;

    const float moveSpeed = 0.1f * m2v / state.viewParams.scaleFactor;
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
    XrSwapchain swapchain;
    int64_t format;
    uint32_t width, height, imageCount;
    std::vector<GLuint> images;
};

struct AppXrSession {
    XrInstance instance;
    XrSystemId systemId;
    XrSession session;
    XrSpace localSpace;
    XrSpace viewSpace;
    SwapchainInfo swapchain;
    XrViewConfigurationType viewConfigType;
    std::vector<XrViewConfigurationView> configViews;
    XrSessionState sessionState;
    bool sessionRunning;
    bool exitRequested;
    bool hasCocoaWindowBinding;

    // XR_EXT_display_info
    bool hasDisplayInfoExt;
    float displayWidthM;
    float displayHeightM;
    float nominalViewerX, nominalViewerY, nominalViewerZ;
    uint32_t displayPixelWidth, displayPixelHeight;
    float recommendedViewScaleX, recommendedViewScaleY;
    PFN_xrRequestDisplayModeEXT pfnRequestDisplayModeEXT;
    PFN_xrRequestDisplayRenderingModeEXT pfnRequestDisplayRenderingModeEXT;
    PFN_xrEnumerateDisplayRenderingModesEXT pfnEnumerateDisplayRenderingModesEXT;

    // XR_EXT_atlas_capture (W6 of #396): runtime-owned 'I'-key atlas capture.
    bool hasAtlasCaptureExt = false;
    bool hasViewRigExt = false;  // XR_EXT_view_rig (#396 W7)
    PFN_xrCaptureAtlasEXT pfnCaptureAtlasEXT = nullptr;

    // XR_EXT_local_3d_zone (#439 Phase 3, cases 2/3/4)
    bool hasLocal3DZoneExt = false;
    PFN_xrCreateLocal3DZoneMaskEXT pfnCreateLocal3DZoneMaskEXT = nullptr;
    PFN_xrSetLocal3DZoneFromRectsEXT pfnSetLocal3DZoneFromRectsEXT = nullptr;
    PFN_xrSubmitLocal3DZoneEXT pfnSubmitLocal3DZoneEXT = nullptr;
    PFN_xrDestroyLocal3DZoneMaskEXT pfnDestroyLocal3DZoneMaskEXT = nullptr;
    XrLocal3DZoneMaskEXT local3DZoneMask = XR_NULL_HANDLE;

    // Enumerated rendering mode info. currentModeIndex is initialized to mode 1
    // as a fallback for runtimes that don't expose isActive; v13+ runtimes
    // replace it via the enumerate step (initial-mode-sync, #234/#239).
    uint32_t currentModeIndex = 1;
    uint32_t renderingModeCount;
    char renderingModeNames[8][XR_MAX_SYSTEM_NAME_SIZE];
    uint32_t renderingModeViewCounts[8] = {};
    uint32_t renderingModeTileColumns[8] = {};
    uint32_t renderingModeTileRows[8] = {};
    float renderingModeScaleX[8] = {};
    float renderingModeScaleY[8] = {};
    bool renderingModeDisplay3D[8] = {};
    bool renderingModeIsRequestable[8] = {};  // v13: false when workspace-locked

    // Eye tracking
    float eyePositions[8][3] = {};  // [view][x,y,z] — raw per-eye positions in display space
    uint32_t eyeCount = 0;          // Number of valid eye positions
    bool isEyeTracking;

    char systemName[XR_MAX_SYSTEM_NAME_SIZE];
};

// ============================================================================
// OpenXR initialization
// ============================================================================

static bool InitializeOpenXR(AppXrSession &app)
{
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES, nullptr, "", 0});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    bool hasGLEnable = false;
    bool hasMacosGlBinding = false;
    app.hasCocoaWindowBinding = false;
    app.hasDisplayInfoExt = false;

    LOG_INFO("Available OpenXR extensions:");
    for (auto &e : exts) {
        LOG_INFO("  %s v%u", e.extensionName, e.extensionVersion);
        if (strcmp(e.extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0)
            hasGLEnable = true;
        if (strcmp(e.extensionName, XR_EXT_MACOS_GL_BINDING_EXTENSION_NAME) == 0)
            hasMacosGlBinding = true;
        if (strcmp(e.extensionName, XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME) == 0)
            app.hasCocoaWindowBinding = true;
        if (strcmp(e.extensionName, XR_EXT_DISPLAY_INFO_EXTENSION_NAME) == 0)
            app.hasDisplayInfoExt = true;
        if (strcmp(e.extensionName, XR_EXT_ATLAS_CAPTURE_EXTENSION_NAME) == 0)
            app.hasAtlasCaptureExt = true;
        if (strcmp(e.extensionName, XR_EXT_LOCAL_3D_ZONE_EXTENSION_NAME) == 0)
            app.hasLocal3DZoneExt = true;
        if (strcmp(e.extensionName, XR_EXT_MCP_TOOLS_EXTENSION_NAME) == 0)
            g_hasMcpToolsExt = true;
        if (strcmp(e.extensionName, XR_EXT_VIEW_RIG_EXTENSION_NAME) == 0)
            app.hasViewRigExt = true;
        if (strcmp(e.extensionName, XR_EXT_DISPLAY_ZONES_EXTENSION_NAME) == 0)
            g_hasDisplayZonesExt = true;
    }

    // XR_EXT_display_zones composes XR_EXT_local_3d_zone + XR_EXT_view_rig —
    // it is only usable when both prerequisites are also available.
    if (g_hasDisplayZonesExt && (!app.hasLocal3DZoneExt || !app.hasViewRigExt)) {
        LOG_ERROR("XR_EXT_display_zones advertised without its prerequisites "
                  "(local_3d_zone=%d view_rig=%d) — zones path disabled",
                  (int)app.hasLocal3DZoneExt, (int)app.hasViewRigExt);
        g_hasDisplayZonesExt = false;
    }
    if (!g_hasDisplayZonesExt) {
        LOG_ERROR("XR_EXT_display_zones NOT available — running as plain single-projection cube");
    }

    if (!hasMacosGlBinding) {
        LOG_ERROR("Runtime does not support XR_EXT_macos_gl_binding — required for this binding");
        return false;
    }
    if (!app.hasCocoaWindowBinding) {
        LOG_WARN("Runtime does not support XR_EXT_cocoa_window_binding — will create own window");
    }
    LOG_INFO("XR_EXT_display_info: %s", app.hasDisplayInfoExt ? "available" : "not available");
    LOG_INFO("XR_EXT_atlas_capture: %s", app.hasAtlasCaptureExt ? "available" : "not available");
    LOG_INFO("XR_EXT_view_rig: %s", app.hasViewRigExt ? "AVAILABLE" : "NOT FOUND");

    // Enable extensions
    std::vector<const char *> enabledExts = {XR_EXT_MACOS_GL_BINDING_EXTENSION_NAME};
    if (hasGLEnable) {
        enabledExts.push_back(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
    }
    if (app.hasCocoaWindowBinding) {
        enabledExts.push_back(XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME);
    }
    if (app.hasDisplayInfoExt) {
        enabledExts.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);
    }
    if (app.hasAtlasCaptureExt) {
        enabledExts.push_back(XR_EXT_ATLAS_CAPTURE_EXTENSION_NAME);
    }
    if (app.hasLocal3DZoneExt) {
        enabledExts.push_back(XR_EXT_LOCAL_3D_ZONE_EXTENSION_NAME);
    }
    if (g_hasMcpToolsExt) {
        enabledExts.push_back(XR_EXT_MCP_TOOLS_EXTENSION_NAME);
    }
    if (app.hasViewRigExt) {
        enabledExts.push_back(XR_EXT_VIEW_RIG_EXTENSION_NAME);
    }
    if (g_hasDisplayZonesExt) {
        enabledExts.push_back(XR_EXT_DISPLAY_ZONES_EXTENSION_NAME);
    }

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(createInfo.applicationInfo.applicationName, "GLCubeZonesTextureOpenXR",
            XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.applicationVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = (uint32_t)enabledExts.size();
    createInfo.enabledExtensionNames = enabledExts.data();

    XR_CHECK(xrCreateInstance(&createInfo, &app.instance));
    LOG_INFO("OpenXR instance created");
    LOG_INFO("XR_EXT_cocoa_window_binding: %s", app.hasCocoaWindowBinding ? "enabled" : "not available");

    // Get system
    XrSystemGetInfo sysInfo = {XR_TYPE_SYSTEM_GET_INFO};
    sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(app.instance, &sysInfo, &app.systemId));
    LOG_INFO("Got system ID: %llu", (unsigned long long)app.systemId);

    // Get system name and display info
    {
        XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayInfoEXT displayInfo = {};
        displayInfo.type = XR_TYPE_DISPLAY_INFO_EXT;
        if (app.hasDisplayInfoExt) {
            sysProps.next = &displayInfo;
        }
        if (XR_SUCCEEDED(xrGetSystemProperties(app.instance, app.systemId, &sysProps))) {
            memcpy(app.systemName, sysProps.systemName, sizeof(app.systemName));
            LOG_INFO("System name: %s", app.systemName);
            if (app.hasDisplayInfoExt) {
                app.displayWidthM = displayInfo.displaySizeMeters.width;
                app.displayHeightM = displayInfo.displaySizeMeters.height;
                app.nominalViewerX = displayInfo.nominalViewerPositionInDisplaySpace.x;
                app.nominalViewerY = displayInfo.nominalViewerPositionInDisplaySpace.y;
                app.nominalViewerZ = displayInfo.nominalViewerPositionInDisplaySpace.z;
                app.displayPixelWidth = displayInfo.displayPixelWidth;
                app.displayPixelHeight = displayInfo.displayPixelHeight;
                app.recommendedViewScaleX = displayInfo.recommendedViewScaleX;
                app.recommendedViewScaleY = displayInfo.recommendedViewScaleY;
                LOG_INFO("Display pixels: %ux%u", app.displayPixelWidth, app.displayPixelHeight);
                LOG_INFO("Display info: %.3fx%.3f m, scale=%.2fx%.2f, nominal=(%.3f,%.3f,%.3f)",
                    app.displayWidthM, app.displayHeightM,
                    app.recommendedViewScaleX, app.recommendedViewScaleY,
                    app.nominalViewerX, app.nominalViewerY, app.nominalViewerZ);
            }
        }
        // Load display extension function pointers
        if (app.hasDisplayInfoExt) {
            xrGetInstanceProcAddr(app.instance, "xrRequestDisplayModeEXT",
                (PFN_xrVoidFunction*)&app.pfnRequestDisplayModeEXT);
            xrGetInstanceProcAddr(app.instance, "xrRequestDisplayRenderingModeEXT",
                (PFN_xrVoidFunction*)&app.pfnRequestDisplayRenderingModeEXT);
            xrGetInstanceProcAddr(app.instance, "xrEnumerateDisplayRenderingModesEXT",
                (PFN_xrVoidFunction*)&app.pfnEnumerateDisplayRenderingModesEXT);
        }
        if (app.hasAtlasCaptureExt) {
            xrGetInstanceProcAddr(app.instance, "xrCaptureAtlasEXT",
                (PFN_xrVoidFunction*)&app.pfnCaptureAtlasEXT);
            LOG_INFO("xrCaptureAtlasEXT: %s", app.pfnCaptureAtlasEXT ? "resolved" : "NULL");
        }
        if (app.hasLocal3DZoneExt) {
            xrGetInstanceProcAddr(app.instance, "xrCreateLocal3DZoneMaskEXT",
                (PFN_xrVoidFunction*)&app.pfnCreateLocal3DZoneMaskEXT);
            xrGetInstanceProcAddr(app.instance, "xrSetLocal3DZoneFromRectsEXT",
                (PFN_xrVoidFunction*)&app.pfnSetLocal3DZoneFromRectsEXT);
            xrGetInstanceProcAddr(app.instance, "xrSubmitLocal3DZoneEXT",
                (PFN_xrVoidFunction*)&app.pfnSubmitLocal3DZoneEXT);
            xrGetInstanceProcAddr(app.instance, "xrDestroyLocal3DZoneMaskEXT",
                (PFN_xrVoidFunction*)&app.pfnDestroyLocal3DZoneMaskEXT);
        }
        if (g_hasDisplayZonesExt) {
            xrGetInstanceProcAddr(app.instance, "xrGetDisplayZoneCapabilitiesEXT",
                (PFN_xrVoidFunction*)&g_pfnGetZoneCaps);
            xrGetInstanceProcAddr(app.instance, "xrGetDisplayZoneRecommendedViewSizeEXT",
                (PFN_xrVoidFunction*)&g_pfnGetZoneViewSize);
            if (g_pfnGetZoneCaps == nullptr || g_pfnGetZoneViewSize == nullptr) {
                LOG_ERROR("XR_EXT_display_zones entry points unresolved — zones path disabled");
                g_hasDisplayZonesExt = false;
            }
        }
    }

    // Enumerate view configs
    app.viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(app.instance, app.systemId, app.viewConfigType,
                                                0, &viewCount, nullptr));
    app.configViews.resize(viewCount);
    for (uint32_t i = 0; i < viewCount; i++) {
        app.configViews[i] = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
    }
    XR_CHECK(xrEnumerateViewConfigurationViews(app.instance, app.systemId, app.viewConfigType,
                                                viewCount, &viewCount, app.configViews.data()));
    LOG_INFO("View configuration: %u views", viewCount);
    for (uint32_t i = 0; i < viewCount; i++) {
        LOG_INFO("  View %u: recommended %ux%u", i,
                 app.configViews[i].recommendedImageRectWidth,
                 app.configViews[i].recommendedImageRectHeight);
    }

    return true;
}

static bool GetGLGraphicsRequirements(AppXrSession &app)
{
    PFN_xrGetOpenGLGraphicsRequirementsKHR xrGetOpenGLGraphicsRequirementsKHR = nullptr;
    XrResult res = xrGetInstanceProcAddr(app.instance, "xrGetOpenGLGraphicsRequirementsKHR",
                                          (PFN_xrVoidFunction *)&xrGetOpenGLGraphicsRequirementsKHR);

    if (XR_FAILED(res) || xrGetOpenGLGraphicsRequirementsKHR == nullptr) {
        LOG_WARN("xrGetOpenGLGraphicsRequirementsKHR not available (using macos_gl_binding only)");
        return true;
    }

    XrGraphicsRequirementsOpenGLKHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    res = xrGetOpenGLGraphicsRequirementsKHR(app.instance, app.systemId, &reqs);
    if (XR_SUCCEEDED(res)) {
        LOG_INFO("OpenGL graphics requirements: min=%u.%u max=%u.%u",
                 XR_VERSION_MAJOR(reqs.minApiVersionSupported),
                 XR_VERSION_MINOR(reqs.minApiVersionSupported),
                 XR_VERSION_MAJOR(reqs.maxApiVersionSupported),
                 XR_VERSION_MINOR(reqs.maxApiVersionSupported));
    }
    return true;
}

static bool CreateSession(AppXrSession &app, GLRenderer &r)
{
    (void)r;
    LOG_INFO("Creating OpenXR session with macOS GL binding + cocoa_window_binding...");

    // Get CGL context from our NSOpenGLContext
    CGLContextObj cglCtx = (CGLContextObj)[g_glContext CGLContextObj];

    XrGraphicsBindingOpenGLMacOSEXT glBinding = {};
    glBinding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_MACOS_EXT;
    glBinding.cglContext = (void *)cglCtx;
    glBinding.cglPixelFormat = nullptr;

    // Chain the cocoa window binding extension — pass our NSView to the runtime
    XrCocoaWindowBindingCreateInfoEXT cocoaBinding = {};
    cocoaBinding.type = XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT;
    cocoaBinding.next = nullptr;
    cocoaBinding.viewHandle = (__bridge void *)g_glView;
    // TEXTURE-MODE MARKER: hand the runtime our shared IOSurface so it
    // composites into it (the runtime does NOT present — the app does). The
    // real NSView is still passed for DP screen-space position tracking, as in
    // cube_texture_metal_macos. The IOSurface is created before xrCreateSession
    // in main().
    cocoaBinding.sharedIOSurface = (void *)g_ioSurface;
    LOG_INFO("Texture mode: chaining sharedIOSurface=%p with viewHandle=%p",
             (void *)g_ioSurface, cocoaBinding.viewHandle);

    // Transparent-background mode (spec v5) — DEFAULT ON for the zones demo:
    // zones alpha-composite against the desktop by design (translucent zone
    // backgrounds, content-alpha edge fades, transparent unzoned regions).
    // Opt out with DISPLAYXR_TRANSPARENT_BG=0.
    {
        const char *e = getenv("DISPLAYXR_TRANSPARENT_BG");
        if (e == nullptr || *e != '0') {
            cocoaBinding.transparentBackgroundEnabled = XR_TRUE;
            // Make our app-owned NSWindow non-opaque too — the runtime only
            // configures the layer it presents into for app-provided views;
            // the NSWindow alpha is the app's responsibility.
            if (g_glView != nil && g_glView.window != nil) {
                [g_glView.window setOpaque:NO];
                [g_glView.window setBackgroundColor:[NSColor clearColor]];
            }
            LOG_INFO("Transparent background ENABLED (zones default; DISPLAYXR_TRANSPARENT_BG=0 to opt out)");
        }
    }

    if (app.hasCocoaWindowBinding) {
        glBinding.next = &cocoaBinding;
        LOG_INFO("Chaining XR_EXT_cocoa_window_binding with NSView %p", cocoaBinding.viewHandle);
    }

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &glBinding;
    sessionInfo.systemId = app.systemId;

    XR_CHECK(xrCreateSession(app.instance, &sessionInfo, &app.session));
    LOG_INFO("Session created%s", app.hasCocoaWindowBinding ? " (with external window)" : "");

    // XR_EXT_mcp_tools: declare identity + register agent tools. The appId
    // MUST match `id` in displayxr/cube_zones_texture_gl_macos.displayxr.json
    // (INV-10.1). Failure is non-fatal by design — the MCP capability gate
    // may simply be off on this machine.
    if (g_hasMcpToolsExt) {
        xrGetInstanceProcAddr(app.instance, "xrSetMCPAppInfoEXT",
                              (PFN_xrVoidFunction *)&g_pfnSetMCPAppInfo);
        xrGetInstanceProcAddr(app.instance, "xrRegisterMCPToolEXT",
                              (PFN_xrVoidFunction *)&g_pfnRegisterMCPTool);
        xrGetInstanceProcAddr(app.instance, "xrGetMCPToolCallArgsEXT",
                              (PFN_xrVoidFunction *)&g_pfnGetMCPToolCallArgs);
        xrGetInstanceProcAddr(app.instance, "xrSubmitMCPToolResultEXT",
                              (PFN_xrVoidFunction *)&g_pfnSubmitMCPToolResult);
        if (g_pfnSetMCPAppInfo && g_pfnRegisterMCPTool && g_pfnSubmitMCPToolResult) {
            XrMCPAppInfoEXT mcpAppInfo = {XR_TYPE_MCP_APP_INFO_EXT};
            strncpy(mcpAppInfo.appId, "cube-zones-texture-gl", sizeof(mcpAppInfo.appId) - 1);
            XrResult ar = g_pfnSetMCPAppInfo(app.session, &mcpAppInfo);

            XrMCPToolInfoEXT setSpin = {XR_TYPE_MCP_TOOL_INFO_EXT};
            setSpin.name = "set_spin";
            setSpin.description =
                "Set the cube's spin speed. Takes effect immediately; the change is "
                "visually verifiable via capture_frame. Returns the applied speed.";
            setSpin.inputSchemaJson =
                "{\"type\":\"object\",\"properties\":{\"speed_rad_per_sec\":{\"type\":\"number\","
                "\"minimum\":0,\"maximum\":10,\"description\":\"Spin speed in radians/second; "
                "0 freezes the cube. Default at launch is 0.5.\"}},"
                "\"required\":[\"speed_rad_per_sec\"]}";
            XrResult tr1 = g_pfnRegisterMCPTool(app.session, &setSpin);

            XrMCPToolInfoEXT getStatus = {XR_TYPE_MCP_TOOL_INFO_EXT};
            getStatus.name = "get_status";
            getStatus.description =
                "Read the cube app's live state: spin speed (rad/s), whether the XR "
                "session is running, and the active rendering-mode index.";
            getStatus.inputSchemaJson = "{\"type\":\"object\"}";
            XrResult tr2 = g_pfnRegisterMCPTool(app.session, &getStatus);

            LOG_INFO("XR_EXT_mcp_tools: appId=%d set_spin=%d get_status=%d", ar, tr1, tr2);
        }
    }

    // Enumerate available rendering modes and store names
    app.renderingModeCount = 0;
    if (app.pfnEnumerateDisplayRenderingModesEXT && app.session != XR_NULL_HANDLE) {
        uint32_t modeCount = 0;
        XrResult enumRes = app.pfnEnumerateDisplayRenderingModesEXT(app.session, 0, &modeCount, nullptr);
        if (XR_SUCCEEDED(enumRes) && modeCount > 0) {
            std::vector<XrDisplayRenderingModeInfoEXT> modes(modeCount);
            for (uint32_t i = 0; i < modeCount; i++) {
                modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT;
                modes[i].next = nullptr;
            }
            enumRes = app.pfnEnumerateDisplayRenderingModesEXT(app.session, modeCount, &modeCount, modes.data());
            if (XR_SUCCEEDED(enumRes)) {
                app.renderingModeCount = modeCount > 8 ? 8 : modeCount;
                LOG_INFO("Display rendering modes (%u):", modeCount);
                for (uint32_t i = 0; i < app.renderingModeCount; i++) {
                    strncpy(app.renderingModeNames[i], modes[i].modeName, XR_MAX_SYSTEM_NAME_SIZE - 1);
                    app.renderingModeNames[i][XR_MAX_SYSTEM_NAME_SIZE - 1] = '\0';
                    app.renderingModeViewCounts[i] = modes[i].viewCount;
                    app.renderingModeTileColumns[i] = modes[i].tileColumns;
                    app.renderingModeTileRows[i] = modes[i].tileRows;
                    app.renderingModeScaleX[i] = modes[i].viewScaleX;
                    app.renderingModeScaleY[i] = modes[i].viewScaleY;
                    app.renderingModeDisplay3D[i] = modes[i].hardwareDisplay3D ? true : false;
                    app.renderingModeIsRequestable[i] = modes[i].isRequestable ? true : false;
                    // v13 initial-mode-sync: trust runtime-reported active mode.
                    if (modes[i].isActive) {
                        app.currentModeIndex = modes[i].modeIndex;
                    }
                    LOG_INFO("  [%u] %s (views=%u, tiles=%ux%u, scale=%.2fx%.2f, 3D=%s)",
                        modes[i].modeIndex, modes[i].modeName,
                        modes[i].viewCount, modes[i].tileColumns, modes[i].tileRows,
                        modes[i].viewScaleX, modes[i].viewScaleY,
                        modes[i].hardwareDisplay3D ? "yes" : "no");
                }
                g_input.renderingModeCount = app.renderingModeCount;
            }
        }
    }

    app.sessionState = XR_SESSION_STATE_UNKNOWN;
    app.sessionRunning = false;
    app.exitRequested = false;
    return true;
}

static bool CreateSpaces(AppXrSession &app)
{
    XrReferenceSpaceCreateInfo spaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace = {{0,0,0,1}, {0,0,0}};
    XR_CHECK(xrCreateReferenceSpace(app.session, &spaceInfo, &app.localSpace));

    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(app.session, &spaceInfo, &app.viewSpace));

    LOG_INFO("Reference spaces created");
    return true;
}

static bool CreateSwapchain(AppXrSession &app)
{
    // Size swapchain for the maximum atlas across all rendering modes.
    // Each mode's atlas is: (tileColumns * scaleX * displayW) × (tileRows * scaleY * displayH).
    uint32_t w = app.configViews[0].recommendedImageRectWidth * 2;  // fallback: stereo SBS
    uint32_t h = app.configViews[0].recommendedImageRectHeight;
    if (app.renderingModeCount > 0 && app.displayPixelWidth > 0 && app.displayPixelHeight > 0) {
        w = 0; h = 0;
        for (uint32_t i = 0; i < app.renderingModeCount; i++) {
            uint32_t mw = (uint32_t)(app.renderingModeTileColumns[i] * app.renderingModeScaleX[i] * app.displayPixelWidth);
            uint32_t mh = (uint32_t)(app.renderingModeTileRows[i] * app.renderingModeScaleY[i] * app.displayPixelHeight);
            if (mw > w) w = mw;
            if (mh > h) h = mh;
        }
    }

    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(app.session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(app.session, formatCount, &formatCount, formats.data()));

    // GL swapchain formats: prefer GL_SRGB8_ALPHA8 (0x8C43), fall back to
    // GL_RGBA8 (0x8058), else formats[0].
    LOG_INFO("Supported swapchain formats:");
    int64_t selectedFormat = formats[0];
    bool haveSrgb = false, haveRgba8 = false;
    for (auto f : formats) {
        if (f == 0x8C43) haveSrgb = true;
        if (f == 0x8058) haveRgba8 = true;
    }
    if (haveSrgb) selectedFormat = 0x8C43;
    else if (haveRgba8) selectedFormat = 0x8058;
    for (auto f : formats) {
        LOG_INFO("  format 0x%llx%s", (long long)f, f == selectedFormat ? " (selected)" : "");
    }

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = selectedFormat;
    sci.sampleCount = 1;
    sci.width = w;
    sci.height = h;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;

    XR_CHECK(xrCreateSwapchain(app.session, &sci, &app.swapchain.swapchain));
    app.swapchain.format = selectedFormat;
    app.swapchain.width = w;
    app.swapchain.height = h;

    uint32_t imageCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(app.swapchain.swapchain, 0, &imageCount, nullptr));
    app.swapchain.imageCount = imageCount;

    std::vector<XrSwapchainImageOpenGLKHR> glImages(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(app.swapchain.swapchain, imageCount, &imageCount,
                                         (XrSwapchainImageBaseHeader *)glImages.data()));

    app.swapchain.images.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        app.swapchain.images[i] = glImages[i].image;
        LOG_INFO("Swapchain image %u: GL texture %u", i, glImages[i].image);
    }

    LOG_INFO("Swapchain created: %ux%u, %u images", w, h, imageCount);
    return true;
}

// ============================================================================
// #439 Phase 3 — Local2D panel swapchains (cases 2/3/4)
// ============================================================================

struct L2DPanel {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t w = 0, h = 0;
};
static L2DPanel g_panel1, g_panel2;
static XrRect2Di g_panel1Rect, g_panel2Rect;

// Create a window-anchored Local2D panel swapchain and fill it once (static
// content: acquire/fill/release once; the layer references the released
// image every frame).
//  variant 0 — crispness panel: opaque fine 8-px checker core with a 24-px
//              half-transparent green border (PREMULTIPLIED bytes), so the
//              border resolves against the desktop where M=0.
//  variant 1 — stacking/alpha panel: UNPREMULTIPLIED orange at a=128 with
//              opaque white diagonal stripes; submitted with
//              XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT (fringing
//              check for the SrcAlpha flatten path).
static bool CreateAndFillL2DPanel(AppXrSession &app, uint32_t w, uint32_t h, int variant, L2DPanel &out)
{
    if (w == 0 || h == 0) {
        return false;
    }

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = app.swapchain.format;
    sci.sampleCount = 1;
    sci.width = w;
    sci.height = h;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(app.session, &sci, &out.swapchain))) {
        return false;
    }
    out.w = w;
    out.h = h;

    uint32_t n = 0;
    xrEnumerateSwapchainImages(out.swapchain, 0, &n, nullptr);
    std::vector<XrSwapchainImageOpenGLKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
    if (n == 0 || XR_FAILED(xrEnumerateSwapchainImages(out.swapchain, n, &n,
                                                       (XrSwapchainImageBaseHeader *)imgs.data()))) {
        return false;
    }

    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t idx = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(out.swapchain, &ai, &idx))) {
        return false;
    }
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(out.swapchain, &wi);

    size_t stride = (size_t)w * 4;
    uint8_t *buf = (uint8_t *)malloc(stride * h);
    if (buf != NULL) {
        const uint32_t border = 24;
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *row = buf + (size_t)y * stride;
            for (uint32_t x = 0; x < w; x++) {
                uint8_t *px = row + (size_t)x * 4;
                if (variant == 0) {
                    bool inBorder = (x < border || y < border || x >= w - border || y >= h - border);
                    if (inBorder) {
                        // Half-transparent green, PREMULTIPLIED bytes.
                        px[0] = 0;
                        px[1] = 128;
                        px[2] = 0;
                        px[3] = 128;
                    } else {
                        // Opaque fine checker (crispness probe).
                        bool check = (((x / 8) + (y / 8)) & 1) != 0;
                        uint8_t v = check ? 235 : 40;
                        px[0] = v;
                        px[1] = v;
                        px[2] = v;
                        px[3] = 255;
                    }
                } else {
                    bool stripe = (((x + y) / 16) & 1) != 0;
                    if (stripe) {
                        // Opaque white stripes.
                        px[0] = 255;
                        px[1] = 255;
                        px[2] = 255;
                        px[3] = 255;
                    } else {
                        // UNPREMULTIPLIED orange at a=128 — channels carry
                        // the full color; the compositor's SrcAlpha blend
                        // does the multiply.
                        px[0] = 0;
                        px[1] = 165;
                        px[2] = 255;
                        px[3] = 128;
                    }
                }
            }
        }
        UploadBGRAToSwapchainTex(imgs[idx].image, w, h, buf, 0);
        free(buf);
    }

    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(out.swapchain, &ri);
    return true;
}

// ============================================================================
// XR_EXT_display_zones helpers (ADR-027) — port of cube_zones_d3d11_win
// ============================================================================

static const float kZoneEdgeFadePx = 16.0f;

// Create the always-on Local2D strip swapchain and fill it once (static
// content: acquire/fill/release once; the layer references the released image
// every frame). Checker + a solid amber label band; OPAQUE alpha throughout.
static bool CreateAndFillStrip(AppXrSession &app)
{
    const uint32_t w = (uint32_t)g_stripRect.extent.width;
    const uint32_t h = (uint32_t)g_stripRect.extent.height;
    if (w == 0 || h == 0) {
        return false;
    }

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = app.swapchain.format;
    sci.sampleCount = 1;
    sci.width = w;
    sci.height = h;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(app.session, &sci, &g_strip.swapchain))) {
        LOG_ERROR("[zones] strip: xrCreateSwapchain failed");
        return false;
    }
    g_strip.w = w;
    g_strip.h = h;

    uint32_t n = 0;
    xrEnumerateSwapchainImages(g_strip.swapchain, 0, &n, nullptr);
    std::vector<XrSwapchainImageOpenGLKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
    if (n == 0 || XR_FAILED(xrEnumerateSwapchainImages(g_strip.swapchain, n, &n,
                                                       (XrSwapchainImageBaseHeader *)imgs.data()))) {
        LOG_ERROR("[zones] strip: xrEnumerateSwapchainImages failed");
        return false;
    }

    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t idx = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(g_strip.swapchain, &ai, &idx))) {
        return false;
    }
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(g_strip.swapchain, &wi);

    // Same content as the D3D11 source app, with the checker cell and amber
    // label band scaled to the backing-pixel strip size (the source strip is
    // 1280x180 logical px).
    const uint32_t cell = (h * 24 / 180) > 8 ? (h * 24 / 180) : 8;
    const uint32_t lx0 = w * 40 / 1280, lx1 = w * 360 / 1280;
    const uint32_t ly0 = h * 70 / 180, ly1 = h * 110 / 180;
    size_t stride = (size_t)w * 4; // BGRA8
    std::vector<uint8_t> buf(stride * h);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = buf.data() + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *px = row + (size_t)x * 4; // B,G,R,A
            bool label = (x >= lx0 && x < lx1 && y >= ly0 && y < ly1);
            if (label) {
                px[0] = 0;   // B
                px[1] = 170; // G
                px[2] = 255; // R
                px[3] = 255;
            } else {
                bool check = (((x / cell) + (y / cell)) & 1) != 0;
                uint8_t v = check ? 210 : 60;
                px[0] = v;
                px[1] = v;
                px[2] = v;
                px[3] = 255;
            }
        }
    }
    UploadBGRAToSwapchainTex(imgs[idx].image, w, h, buf.data(), 0);

    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_strip.swapchain, &ri);
    return true;
}

// Create one zone's swapchain, sized per xrGetDisplayZoneRecommendedViewSizeEXT,
// horizontally tiled per view. The per-zone depth texture is created lazily by
// the render (zone sizes differ, so the shared renderer depth would ping-pong).
static bool CreateZoneResources(AppXrSession &app, DisplayZone &z, uint32_t viewCount)
{
    XrExtent2Di rec = {};
    XrResult r = g_pfnGetZoneViewSize(app.session, &z.rect, &rec);
    if (XR_FAILED(r) || rec.width <= 0 || rec.height <= 0) {
        LOG_ERROR("[zones] zone %u: xrGetDisplayZoneRecommendedViewSizeEXT failed (0x%x, %dx%d)",
                  z.zoneId, (unsigned)r, rec.width, rec.height);
        return false;
    }
    z.tileW = (uint32_t)rec.width;
    z.tileH = (uint32_t)rec.height;
    z.tileCount = viewCount;
    z.format = app.swapchain.format; // same encoding as the main projection swapchain

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = z.format;
    sci.sampleCount = 1;
    sci.width = z.tileW * z.tileCount;
    sci.height = z.tileH;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(app.session, &sci, &z.swapchain))) {
        LOG_ERROR("[zones] zone %u: xrCreateSwapchain failed (%ux%u)", z.zoneId, sci.width, sci.height);
        return false;
    }

    uint32_t n = 0;
    xrEnumerateSwapchainImages(z.swapchain, 0, &n, nullptr);
    std::vector<XrSwapchainImageOpenGLKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
    if (n == 0 || XR_FAILED(xrEnumerateSwapchainImages(z.swapchain, n, &n,
                                                       (XrSwapchainImageBaseHeader *)imgs.data()))) {
        LOG_ERROR("[zones] zone %u: xrEnumerateSwapchainImages failed", z.zoneId);
        return false;
    }
    z.images.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        z.images[i] = imgs[i].image;
    }

    LOG_INFO("[zones] zone %u: rect %d,%d %dx%d -> swapchain %ux%u (%u tiles of %ux%u)",
             z.zoneId, z.rect.offset.x, z.rect.offset.y, z.rect.extent.width, z.rect.extent.height,
             z.tileW * z.tileCount, z.tileH, z.tileCount, z.tileW, z.tileH);
    return true;
}

static void ApplyWishAuthoring(AppXrSession &app);
static const char* WishModeName(int mode);

// One-time zones activation: capabilities check + per-zone swapchains + strip.
// On any failure the zones path is permanently disabled (plain fallback).
static void TryActivateZones(AppXrSession &app)
{
    // Rects are resolved from the live window backing size — retry next frame
    // if the window metrics haven't settled yet.
    const int32_t W = (int32_t)g_windowW;
    const int32_t H = (int32_t)g_windowH;
    if (W <= 0 || H <= 0) {
        return;
    }
    g_zonesAttempted = true;

    XrDisplayZoneCapabilitiesEXT caps = {XR_TYPE_DISPLAY_ZONE_CAPABILITIES_EXT};
    XrResult r = g_pfnGetZoneCaps(app.session, &caps);
    if (XR_FAILED(r) || !caps.supported) {
        LOG_ERROR("[zones] xrGetDisplayZoneCapabilitiesEXT: rc=0x%x supported=%d — zones path disabled",
                  (unsigned)r, (int)caps.supported);
        g_hasDisplayZonesExt = false;
        return;
    }
    if (caps.maxZones3D < kNumZones) {
        LOG_ERROR("[zones] maxZones3D=%u < %u — zones path disabled", caps.maxZones3D, kNumZones);
        g_hasDisplayZonesExt = false;
        return;
    }
    LOG_INFO("[zones] capabilities: supported=1 maxZones3D=%u", caps.maxZones3D);

    // D3D11 source-app proportions of its 1280x720 window, applied to the
    // live backing size.
    g_stripRect        = {{0, 0}, {W, H / 4}};
    g_zoneARect        = {{0, H / 4}, {W / 2, H * 3 / 4}};
    g_zoneBRect        = {{W * 700 / 1280, H / 4}, {W * 520 / 1280, H / 2}};
    g_zoneBOverlapRect = {{W * 400 / 1280, H * 300 / 720}, {W * 520 / 1280, H / 2}};

    // Zones share the session's view COUNT (display modes are session-global);
    // only per-view dimensions vary by rect.
    uint32_t viewCount = 2;
    if (app.renderingModeCount > 0 && app.currentModeIndex < app.renderingModeCount) {
        viewCount = app.renderingModeViewCounts[app.currentModeIndex];
    }
    if (viewCount < 1) viewCount = 1;
    if (viewCount > 8) viewCount = 8;

    // Zone A: left, below the strip. Identity rig, phase 0, dark-red clear.
    g_zonesArr[0].zoneId = 1;
    g_zonesArr[0].rect = g_zoneARect;
    g_zonesArr[0].ipdFactor = 1.0f;
    g_zonesArr[0].perspectiveFactor = 1.0f;
    g_zonesArr[0].spinPhase = 0.0f;
    g_zonesArr[0].clearColor[0] = 0.15f;
    g_zonesArr[0].clearColor[1] = 0.03f;
    g_zonesArr[0].clearColor[2] = 0.03f;
    g_zonesArr[0].clearColor[3] = 1.0f;

    // Zone B: right. Reduced view spread + flattened perspective (visibly
    // different framing), phase +1.5 rad, SEMI-TRANSPARENT dark-blue clear
    // (premultiplied, alpha 0.55): zones composite alpha-over in layer-list
    // order with blends expressed through CONTENT alpha (ADR-027 rule 4) —
    // an opaque background would make the overlap a plain replacement. The
    // cube itself stays opaque; the background lets zone A (and the desktop,
    // when not overlapping) show through.
    g_zonesArr[1].zoneId = 2;
    g_zonesArr[1].rect = g_zoneBOverlap ? g_zoneBOverlapRect : g_zoneBRect;
    g_zonesArr[1].ipdFactor = 0.6f;
    g_zonesArr[1].perspectiveFactor = 0.5f;
    g_zonesArr[1].spinPhase = 1.5f;
    g_zonesArr[1].clearColor[0] = 0.03f * 0.55f;
    g_zonesArr[1].clearColor[1] = 0.03f * 0.55f;
    g_zonesArr[1].clearColor[2] = 0.15f * 0.55f;
    g_zonesArr[1].clearColor[3] = 0.55f;

    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        if (!CreateZoneResources(app, g_zonesArr[zi], viewCount)) {
            g_hasDisplayZonesExt = false;
            return;
        }
    }

    if (!CreateAndFillStrip(app)) {
        g_hasDisplayZonesExt = false;
        return;
    }

    g_zonesActive = true;
    g_input.hudVisible = false; // keep the layer list zones-only
    if (g_wishMode == 1) {
        // DXR_ZONES_WISH_MODE=1 preselected the explicit Tier-2 wish —
        // author it now that the rects are resolved.
        ApplyWishAuthoring(app);
    }
    LOG_INFO("[zones] ACTIVE: zone A %d,%d %dx%d + zone B %d,%d %dx%d + strip %d,%d %dx%d "
             "(views=%u, wish mode %d %s, validate=%d) — M=wish mode, O=overlap toggle",
             g_zoneARect.offset.x, g_zoneARect.offset.y, g_zoneARect.extent.width, g_zoneARect.extent.height,
             g_zonesArr[1].rect.offset.x, g_zonesArr[1].rect.offset.y,
             g_zonesArr[1].rect.extent.width, g_zonesArr[1].rect.extent.height,
             g_stripRect.offset.x, g_stripRect.offset.y, g_stripRect.extent.width, g_stripRect.extent.height,
             viewCount, g_wishMode, WishModeName(g_wishMode), ZonesValidateEnabled() ? 1 : 0);
}

// Lazily create the one mask handle used by wish mode 1 (Tier-2 rects).
static bool EnsureWishMask(AppXrSession &app)
{
    if (app.local3DZoneMask != XR_NULL_HANDLE) return true;
    if (app.pfnCreateLocal3DZoneMaskEXT == nullptr) return false;
    XrLocal3DZoneMaskCreateInfoEXT mci = {(XrStructureType)XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_EXT};
    mci.maskWidth = 0; // runtime picks the window backing size
    mci.maskHeight = 0;
    XrResult r = app.pfnCreateLocal3DZoneMaskEXT(app.session, &mci, &app.local3DZoneMask);
    if (XR_FAILED(r)) {
        LOG_ERROR("[zones] xrCreateLocal3DZoneMaskEXT failed (0x%x)", (unsigned)r);
        app.local3DZoneMask = XR_NULL_HANDLE;
        return false;
    }
    LOG_INFO("[zones] wish mask created (window backing size)");
    return true;
}

// Re-author the wish for the current mode (entering mode 1, or after an O
// rect toggle while in mode 1). Mode 0 authors nothing (AUTO).
static void ApplyWishAuthoring(AppXrSession &app)
{
    if (g_wishMode == 1) {
        if (!EnsureWishMask(app)) return;
        XrRect2Di rects[kNumZones];
        for (uint32_t zi = 0; zi < kNumZones; zi++) rects[zi] = g_zonesArr[zi].rect;
        XrResult r = app.pfnSetLocal3DZoneFromRectsEXT(app.local3DZoneMask, kNumZones, rects);
        if (XR_FAILED(r)) {
            LOG_ERROR("[zones] xrSetLocal3DZoneFromRectsEXT failed (0x%x)", (unsigned)r);
        }
    }
}

static const char* WishModeName(int mode)
{
    switch (mode) {
    case 1: return "explicit Tier-2 rects";
    default: return "AUTO";
    }
}

// Edge-triggered M (wish mode cycle) + O (zone B overlap toggle), set by the
// macOS event pump.
static void HandleZoneKeys(AppXrSession &app)
{
    if (g_wishModeCycleRequested) {
        g_wishModeCycleRequested = false;
        // Tier-3 (freeform render-target wish) has no Metal binding struct —
        // cycle 0 AUTO <-> 1 explicit Tier-2 rects only.
        g_wishMode = (g_wishMode + 1) % 2;
        LOG_INFO("[zones] wish mode %d (%s)", g_wishMode, WishModeName(g_wishMode));
        ApplyWishAuthoring(app);
    }

    if (g_overlapToggleRequested) {
        g_overlapToggleRequested = false;
        g_zoneBOverlap = !g_zoneBOverlap;
        g_zonesArr[1].rect = g_zoneBOverlap ? g_zoneBOverlapRect : g_zoneBRect;
        LOG_INFO("[zones] zone B rect -> %d,%d %dx%d (%s zone A)",
                 g_zonesArr[1].rect.offset.x, g_zonesArr[1].rect.offset.y,
                 g_zonesArr[1].rect.extent.width, g_zonesArr[1].rect.extent.height,
                 g_zoneBOverlap ? "OVERLAPPING" : "beside");
        // Keep an explicit wish in sync with the moved rect.
        ApplyWishAuthoring(app);
    }
}

// Per-zone FBO + depth: zone sizes differ between zones, so the renderer's
// shared depth renderbuffer would ping-pong if reused here. Each zone keeps
// its own FBO + depth renderbuffer.
static void EnsureZoneDepth(GLRenderer &r, DisplayZone &z, uint32_t w, uint32_t h)
{
    (void)r;
    if (z.fbo == 0) {
        glGenFramebuffers(1, &z.fbo);
    }
    if (z.depthRBO == 0 || z.depthW != w || z.depthH != h) {
        if (z.depthRBO) glDeleteRenderbuffers(1, &z.depthRBO);
        glGenRenderbuffers(1, &z.depthRBO);
        glBindRenderbuffer(GL_RENDERBUFFER, z.depthRBO);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, w, h);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        z.depthW = w;
        z.depthH = h;
    }
}

// Render one zone's swapchain image: per-zone clear (premultiplied RGBA),
// cube + grid per view tile with the zone's spin phase, then the
// content-alpha edge fade (ADR-027 rule 4) per tile — the zone blends softly
// into whatever is behind it, desktop OR another zone (the union wish mask
// cannot express per-zone fades inside an overlap).
static void RenderZoneScene(GLRenderer &r, GLuint targetTex, uint32_t targetW, uint32_t targetH,
                            DisplayZone &z, const EyeRenderParams *eyes, int eyeCount)
{
    EnsureZoneDepth(r, z, targetW, targetH);

    glBindFramebuffer(GL_FRAMEBUFFER, z.fbo);
    GLenum texTarget = DetectSwapchainTexTarget(targetTex);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texTarget, targetTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, z.depthRBO);

    GLenum fbStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
        static bool warned = false;
        if (!warned) { warned = true; LOG_ERROR("[zones] FBO incomplete: 0x%x", fbStatus); }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    // Per-zone clear (premultiplied RGBA — the zone background carries alpha).
    // One depth clear per RenderZoneScene call covers all view tiles in the
    // zone's image (matches the Metal loadAction=Clear once-per-pass behavior).
    glViewport(0, 0, (GLsizei)targetW, (GLsizei)targetH);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(z.clearColor[0], z.clearColor[1], z.clearColor[2], z.clearColor[3]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);

    const float zoneRotation = r.cubeRotation + z.spinPhase;

    for (int e = 0; e < eyeCount; e++) {
        const EyeRenderParams &eye = eyes[e];
        glViewport(eye.viewportX, eye.viewportY, eye.width, eye.height);
        glScissor(eye.viewportX, eye.viewportY, eye.width, eye.height);
        glEnable(GL_SCISSOR_TEST);

        float vp_mat[16];
        mat4_multiply(vp_mat, eye.projMat, eye.viewMat);

        // --- Draw cube ---
        {
            const float cubeSize = 0.06f;
            const float cubeHeight = cubeSize / 2.0f;
            float model[16], rotation[16], translation[16], scale[16], tmp[16];
            mat4_scaling(scale, cubeSize);
            mat4_rotation_y(rotation, zoneRotation);
            mat4_translation(translation, 0.0f, cubeHeight, 0.0f);
            mat4_multiply(tmp, scale, rotation);
            mat4_multiply(model, translation, tmp);

            float mvp[16];
            mat4_multiply(mvp, vp_mat, model);

            glUseProgram(r.cubeProgram);
            glUniformMatrix4fv(r.cubeLocMVP, 1, GL_FALSE, mvp);
            glUniformMatrix4fv(r.cubeLocModel, 1, GL_FALSE, model);
            glUniform2f(r.cubeLocTexSize,
                        (float)r.texSizes[0][0], (float)r.texSizes[0][1]);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_RECTANGLE, r.textures[0]);
            glUniform1i(r.cubeLocBasecolor, 0);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_RECTANGLE, r.textures[1]);
            glUniform1i(r.cubeLocNormal, 1);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_RECTANGLE, r.textures[2]);
            glUniform1i(r.cubeLocAO, 2);

            glBindVertexArray(r.cubeVAO);
            glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
        }

        // --- Draw grid ---
        {
            const float gridScale = 0.05f;
            float gridScl[16], gridMvp[16];
            mat4_scaling(gridScl, gridScale);
            mat4_multiply(gridMvp, vp_mat, gridScl);

            glUseProgram(r.gridProgram);
            glUniformMatrix4fv(r.gridLocMVP, 1, GL_FALSE, gridMvp);
            glUniform4f(r.gridLocColor, 0.3f, 0.3f, 0.35f, 1.0f);

            glBindVertexArray(r.gridVAO);
            glDrawArrays(GL_LINES, 0, r.gridVertexCount);
        }
    }

    // --- Content-alpha edge fade per view tile (after the scene) ---
    // Skipped in wish mode 1 (explicit Tier-2 rects): that wish is M=1 out
    // to the exact rect edge, and weave output carries no alpha — content
    // faded inside a hard-M=1 band weaves to opaque black (dark halo), not
    // to the desktop. Tier-2 content must fill its rect to the hard edge.
    if (g_wishMode != 1) {
        // dst *= (1 - src.a) = f, on color AND alpha (premultiplied fade).
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST);

        glUseProgram(r.fadeProgram);
        glUniform2f(r.fadeLocTilePx, (float)z.tileW, (float)z.tileH);
        glUniform2f(r.fadeLocFeatherPx, kZoneEdgeFadePx, 0.0f);
        glBindVertexArray(r.fadeVAO);
        for (int e = 0; e < eyeCount; e++) {
            const EyeRenderParams &eye = eyes[e];
            glViewport(eye.viewportX, eye.viewportY, eye.width, eye.height);
            glScissor(eye.viewportX, eye.viewportY, eye.width, eye.height);
            glEnable(GL_SCISSOR_TEST);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }

    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(0);
    glUseProgram(0);
    glFlush();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Per-frame zones path: zone-scoped locate, per-zone render, submit
// [projA, projB, strip] with the zone structs chained on the projections.
static void RenderZonesFrame(AppXrSession &app, GLRenderer &renderer, const XrFrameState &frameState)
{
    // Per-zone locate + submit data. The zone structs are chained at BOTH
    // points (locate and xrEndFrame) — same instances within the frame.
    XrDisplayZoneEXT zoneStructs[kNumZones];
    XrDisplayRigEXT rigStructs[kNumZones];
    std::vector<XrCompositionLayerProjectionView> projViews[kNumZones];
    uint32_t submitViewCounts[kNumZones] = {};

    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        DisplayZone &z = g_zonesArr[zi];

        rigStructs[zi] = {XR_TYPE_DISPLAY_RIG_EXT};
        rigStructs[zi].pose = {{0, 0, 0, 1}, {0, 0, 0}};
        rigStructs[zi].virtualDisplayHeight = kZoneVirtualDisplayHeight;
        rigStructs[zi].ipdFactor = z.ipdFactor;
        rigStructs[zi].parallaxFactor = 1.0f;
        rigStructs[zi].perspectiveFactor = z.perspectiveFactor;

        zoneStructs[zi] = {XR_TYPE_DISPLAY_ZONE_EXT};
        zoneStructs[zi].next = &rigStructs[zi];
        zoneStructs[zi].zoneId = z.zoneId;
        zoneStructs[zi].rect = z.rect;

        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.next = &zoneStructs[zi];
        locateInfo.viewConfigurationType = app.viewConfigType;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = app.localSpace;

        XrViewState viewState = {XR_TYPE_VIEW_STATE};
        uint32_t viewCountOutput = 0;
        XrView zoneViews[8];
        for (uint32_t vi = 0; vi < 8; vi++) zoneViews[vi] = {XR_TYPE_VIEW};
        XrResult lr = xrLocateViews(app.session, &locateInfo, &viewState, 8, &viewCountOutput, zoneViews);
        if (XR_FAILED(lr) || viewCountOutput == 0) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                LOG_WARN("[zones] zone %u zone-scoped xrLocateViews failed (0x%x)", z.zoneId, (unsigned)lr);
            }
            submitViewCounts[zi] = 0;
            continue;
        }

        const uint32_t n = viewCountOutput < z.tileCount ? viewCountOutput : z.tileCount;
        submitViewCounts[zi] = n;
        projViews[zi].assign(n, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        // Render-ready views -> matrices. ZDP-anchored clip: near = ez - vH,
        // far = ez + 1000*vH, ez = rig-local eye distance to the zone's
        // virtual display plane (identity rig here, so ez = pose z).
        // mat4_from_xr_fov is GL [-1,1]-native.
        std::vector<EyeRenderParams> eyes(n);
        for (uint32_t vi = 0; vi < n; vi++) {
            const XrView &v = zoneViews[vi];
            mat4_view_from_xr_pose(eyes[vi].viewMat, v.pose);
            const float ez = RigLocalEyeZ(rigStructs[zi].pose, v.pose.position);
            const float vH = kZoneVirtualDisplayHeight;
            const float nearZ = (ez - vH > 0.001f) ? (ez - vH) : 0.001f;
            const float farZ = ez + 1000.0f * vH;
            mat4_from_xr_fov(eyes[vi].projMat, v.fov, nearZ, farZ);
            eyes[vi].viewportX = vi * z.tileW;
            eyes[vi].viewportY = 0;
            eyes[vi].width = z.tileW;
            eyes[vi].height = z.tileH;

            projViews[zi][vi].subImage.swapchain = z.swapchain;
            projViews[zi][vi].subImage.imageRect.offset = {(int32_t)(vi * z.tileW), 0};
            projViews[zi][vi].subImage.imageRect.extent = {(int32_t)z.tileW, (int32_t)z.tileH};
            projViews[zi][vi].subImage.imageArrayIndex = 0;
            projViews[zi][vi].pose = v.pose;
            projViews[zi][vi].fov = v.fov;
        }

        // Acquire this zone's swapchain image and render every view tile.
        XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t imageIndex = 0;
        if (XR_FAILED(xrAcquireSwapchainImage(z.swapchain, &ai, &imageIndex))) {
            submitViewCounts[zi] = 0;
            continue;
        }
        XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(z.swapchain, &wi);

        RenderZoneScene(renderer, z.images[imageIndex], z.tileW * z.tileCount, z.tileH,
                        z, eyes.data(), (int)n);

        XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(z.swapchain, &ri);
    }

    // Layer list: [projA (zone A chained), projB (zone B chained), strip].
    XrCompositionLayerProjection projLayers[kNumZones];
    XrCompositionLayerLocal2DEXT stripLayer = {(XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT};
    const XrCompositionLayerBaseHeader *layers[kNumZones + 1] = {};
    uint32_t layerCount = 0;

    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        if (submitViewCounts[zi] == 0) continue;
        projLayers[zi] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projLayers[zi].next = &zoneStructs[zi]; // SAME instance as the locate chain
        // Content alpha is meaningful (zone B translucent bg + the edge
        // fade): declare source-alpha blending (premultiplied bytes).
        projLayers[zi].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        projLayers[zi].space = app.localSpace;
        projLayers[zi].viewCount = submitViewCounts[zi];
        projLayers[zi].views = projViews[zi].data();
        layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&projLayers[zi];
    }

    if (g_strip.swapchain != XR_NULL_HANDLE) {
        stripLayer.layerFlags = 0; // opaque content
        stripLayer.subImage.swapchain = g_strip.swapchain;
        stripLayer.subImage.imageRect.offset = {0, 0};
        stripLayer.subImage.imageRect.extent = {(int32_t)g_strip.w, (int32_t)g_strip.h};
        stripLayer.subImage.imageArrayIndex = 0;
        stripLayer.rect = g_stripRect;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&stripLayer;
    }

    // Zones alpha-composite against the DESKTOP by design (translucent zone
    // backgrounds, content-alpha edge fades, transparent unzoned regions) —
    // submit ALPHA_BLEND whenever the runtime advertises it. With OPAQUE the
    // whole window floor is black and every translucent pixel reads as
    // "faded to black".
    static XrEnvironmentBlendMode zonesBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    static bool blendModeResolved = false;
    if (!blendModeResolved) {
        blendModeResolved = true;
        XrEnvironmentBlendMode modes[8];
        uint32_t count = 0;
        if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(app.instance, app.systemId,
                                                          app.viewConfigType, 8, &count, modes))) {
            for (uint32_t i = 0; i < count; i++) {
                if (modes[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) {
                    zonesBlendMode = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
                    break;
                }
            }
        }
        if (zonesBlendMode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) {
            LOG_INFO("[zones] runtime advertises ALPHA_BLEND — compositing zones over the desktop");
        } else {
            LOG_WARN("[zones] ALPHA_BLEND not advertised — zones composite over an opaque black window");
        }
    }

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = zonesBlendMode;
    endInfo.layerCount = layerCount;
    endInfo.layers = layers;

    // Per-frame wish reference: absent in AUTO (mode 0) unless validation is
    // requested; in mode 1 the mask is the frame's wish, atomic with the
    // layer set.
    XrDisplayZonesFrameEndInfoEXT zonesEnd = {(XrStructureType)XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_EXT};
    zonesEnd.flags = 0;
    zonesEnd.wishMask = XR_NULL_HANDLE;
    bool chainZonesEnd = false;
    if (g_wishMode >= 1 && app.local3DZoneMask != XR_NULL_HANDLE) {
        zonesEnd.wishMask = app.local3DZoneMask;
        chainZonesEnd = true;
    }
    if (ZonesValidateEnabled()) {
        zonesEnd.flags |= XR_DISPLAY_ZONES_FRAME_END_VALIDATE_BIT_EXT;
        chainZonesEnd = true;
    }
    if (chainZonesEnd) {
        endInfo.next = &zonesEnd;
    }

    xrEndFrame(app.session, &endInfo);
}

// ============================================================================
// Event handling
// ============================================================================

static void PollEvents(AppXrSession &app)
{
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(app.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case (XrStructureType)XR_TYPE_EVENT_DATA_DISPLAY_ZONE_METRICS_CHANGED_EXT: {
            // ADR-027 advisory: per-zone recommended view sizes may have
            // changed. Stale sizes stay correct, just soft (the runtime
            // scaled-blits view tiles to rects) — log-only here.
            LOG_INFO("[zones] display-zone metrics changed (advisory) — zone view sizes may be stale");
            break;
        }
        case (XrStructureType)XR_TYPE_EVENT_DATA_LOCAL_3D_ZONE_VIEW_SIZE_CHANGED_EXT: {
            // #439 Q4 — for a handle app the view dims are window-derived
            // already, so this should stay silent when the mask activates;
            // it fires on window resize. Log-only: our swapchain is
            // worst-case sized and render dims follow g_windowW/H per frame.
            auto *vsEvent = (XrEventDataLocal3DZoneViewSizeChangedEXT *)&event;
            LOG_INFO("Local-3D-zone view size changed: %ux%u",
                vsEvent->recommendedImageRectWidth, vsEvent->recommendedImageRectHeight);
            break;
        }
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto *ssc = (XrEventDataSessionStateChanged *)&event;
            app.sessionState = ssc->state;
            LOG_INFO("Session state changed: %d", ssc->state);

            if (ssc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = app.viewConfigType;
                if (XR_SUCCEEDED(xrBeginSession(app.session, &beginInfo))) {
                    app.sessionRunning = true;
                    LOG_INFO("Session started");
                }
            } else if (ssc->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(app.session);
                app.sessionRunning = false;
                LOG_INFO("Session stopped");
            } else if (ssc->state == XR_SESSION_STATE_EXITING ||
                       ssc->state == XR_SESSION_STATE_LOSS_PENDING) {
                app.exitRequested = true;
            }
            break;
        }
        case (XrStructureType)XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_EXT: {
            auto* modeEvent = (XrEventDataRenderingModeChangedEXT*)&event;
            LOG_INFO("Rendering mode changed: %u -> %u",
                modeEvent->previousModeIndex, modeEvent->currentModeIndex);
            app.currentModeIndex = modeEvent->currentModeIndex;
            break;
        }
        case (XrStructureType)XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_EXT: {
            // Edge-triggered tracking loss/recovery (#441 v14); HUD state
            // also refreshes per-frame from the XrViewEyeTrackingStateEXT chain.
            auto* etEvent = (XrEventDataEyeTrackingStateChangedEXT*)&event;
            LOG_INFO("Eye tracking state changed: isTracking=%s mode=%u",
                etEvent->isTracking == XR_TRUE ? "YES" : "NO",
                (uint32_t)etEvent->activeMode);
            app.isEyeTracking = (etEvent->isTracking == XR_TRUE);
            break;
        }
        case (XrStructureType)XR_TYPE_EVENT_DATA_MCP_TOOL_CALL_EXT: {
            // An agent invoked one of our XR_EXT_mcp_tools tools. Fetch the
            // JSON args (two-call idiom; argsSize is the required capacity),
            // act on app state — we're on the main loop, so no locking —
            // and answer. An unanswered call fails to the agent after ~5 s.
            auto *call = (XrEventDataMCPToolCallEXT *)&event;
            char args[512] = {0};
            uint32_t needed = 0;
            if (g_pfnGetMCPToolCallArgs != nullptr) {
                g_pfnGetMCPToolCallArgs(app.session, call->callId, sizeof(args), &needed, args);
            }
            char result[256];
            XrBool32 ok = XR_TRUE;
            if (strcmp(call->toolName, "set_spin") == 0) {
                // Test apps stay dependency-free: hand-scan the one expected
                // numeric key instead of pulling in a JSON parser.
                const char *key = strstr(args, "\"speed_rad_per_sec\"");
                const char *colon = key != nullptr ? strchr(key, ':') : nullptr;
                if (colon != nullptr) {
                    float speed = strtof(colon + 1, nullptr);
                    if (speed < 0.f) speed = 0.f;
                    if (speed > 10.f) speed = 10.f;
                    g_spinSpeed = speed;
                    snprintf(result, sizeof(result), "{\"spin_speed_rad_per_sec\":%.3f}", g_spinSpeed);
                } else {
                    ok = XR_FALSE;
                    snprintf(result, sizeof(result), "{\"error\":\"missing speed_rad_per_sec\"}");
                }
            } else if (strcmp(call->toolName, "get_status") == 0) {
                snprintf(result, sizeof(result),
                         "{\"spin_speed_rad_per_sec\":%.3f,\"session_running\":%s,"
                         "\"rendering_mode_index\":%u}",
                         g_spinSpeed, app.sessionRunning ? "true" : "false", app.currentModeIndex);
            } else {
                ok = XR_FALSE;
                snprintf(result, sizeof(result), "{\"error\":\"unhandled tool\"}");
            }
            if (g_pfnSubmitMCPToolResult != nullptr) {
                g_pfnSubmitMCPToolResult(app.session, call->callId, ok, result);
            }
            break;
        }
        default: break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    LOG_INFO("=== OpenGL Cube Zones TEXTURE OpenXR (XR_EXT_display_zones parity test, ADR-027) ===");

    // Autonomous-verification dump (DXR_TEXDUMP): reads the shared IOSurface
    // back to a PNG at the warmup frame gate. "1" or empty-but-present →
    // default path; any other value is the literal path.
    {
        const char *e = getenv("DXR_TEXDUMP");
        if (e != NULL) {
            g_texDumpEnabled = true;
            if (e[0] == '\0' || (e[0] == '1' && e[1] == '\0')) {
                g_texDumpPath = "/tmp/zones_texture_readback.png";
            } else {
                g_texDumpPath = e;
            }
            LOG_INFO("DXR_TEXDUMP set — IOSurface readback will dump to %s at frame %ld",
                     g_texDumpPath.c_str(), kTexDumpFrame);
        }
    }

    // Headless validation knobs: preselect the M / O key states.
    {
        const char *e = getenv("DXR_ZONES_WISH_MODE");
        if (e != NULL && *e == '1') {
            g_wishMode = 1;
            LOG_INFO("DXR_ZONES_WISH_MODE=1 — starting in explicit Tier-2 rects wish mode");
        }
        e = getenv("DXR_ZONES_OVERLAP");
        if (e != NULL && *e == '1') {
            g_zoneBOverlap = true;
            LOG_INFO("DXR_ZONES_OVERLAP=1 — zone B starts on the overlap rect");
        }
    }

    // #439 Phase 3 cases 2/3/4 (see the g_l2dPanel globals block).
    {
        const char *e = getenv("DXR_LOCAL2D_PANEL");
        g_l2dPanel = (e != NULL && e[0] != '\0' && e[0] != '0');
        e = getenv("DXR_LOCAL2D_MASK");
        g_l2dMask = (e != NULL && e[0] != '\0' && e[0] != '0');
        e = getenv("DXR_LOCAL2D_PANEL2");
        g_l2dPanel2 = (e != NULL && e[0] != '\0' && e[0] != '0');
        e = getenv("DXR_HUD");
        if (e != NULL && e[0] == '0') {
            g_input.hudVisible = false;
        }
        if (g_l2dPanel) {
            // Keep the layer list simple while panels are live.
            g_input.hudVisible = false;
            LOG_INFO("DXR_LOCAL2D_PANEL=1 — Local2D panel layer%s%s",
                     g_l2dMask ? " + explicit Tier-2 island mask" : " (implicit mask)",
                     g_l2dPanel2 ? " + second unpremultiplied panel" : "");
        }
    }

    // Create the macOS window FIRST (app-owned) — it makes the GL context
    // current, which the renderer init needs. (Metal inited the renderer first;
    // GL must reorder so a context exists before any GL call.)
    if (!CreateMacOSWindow(1512, 823)) {
        LOG_ERROR("Failed to create macOS window");
        return 1;
    }

    // Make our context current for renderer init
    [g_glContext makeCurrentContext];

    // Initialize OpenGL renderer
    GLRenderer renderer = {};
    if (!InitRenderer(renderer)) {
        LOG_ERROR("Failed to initialize OpenGL renderer");
        return 1;
    }

    // Initialize OpenXR
    AppXrSession app = {};
    if (!InitializeOpenXR(app)) {
        LOG_ERROR("Failed to initialize OpenXR");
        return 1;
    }

    if (!GetGLGraphicsRequirements(app)) {
        LOG_ERROR("Failed to get OpenGL graphics requirements");
        return 1;
    }

    // TEXTURE MODE: create the shared IOSurface BEFORE the session (the cocoa
    // binding chains it at xrCreateSession). Size it to the worst-case atlas
    // across all rendering modes — same computation as CreateSwapchain and as
    // cube_texture_metal_macos. For the zones path the runtime composites the
    // full-window multi-zone super-atlas into this surface.
    {
        uint32_t ioW = 0, ioH = 0;
        if (app.renderingModeCount > 0 && app.displayPixelWidth > 0 && app.displayPixelHeight > 0) {
            for (uint32_t i = 0; i < app.renderingModeCount; i++) {
                uint32_t mw = (uint32_t)(app.renderingModeTileColumns[i] * app.renderingModeScaleX[i] * app.displayPixelWidth);
                uint32_t mh = (uint32_t)(app.renderingModeTileRows[i] * app.renderingModeScaleY[i] * app.displayPixelHeight);
                if (mw > ioW) ioW = mw;
                if (mh > ioH) ioH = mh;
            }
        }
        if (ioW == 0 || ioH == 0) {
            // Fallback: display dims, else stereo-SBS of the recommended view.
            ioW = app.displayPixelWidth > 0 ? app.displayPixelWidth
                                            : app.configViews[0].recommendedImageRectWidth * 2;
            ioH = app.displayPixelHeight > 0 ? app.displayPixelHeight
                                             : app.configViews[0].recommendedImageRectHeight;
        }
        LOG_INFO("IOSurface dimensions (worst-case atlas): %ux%u", ioW, ioH);
        if (!CreateIOSurface(ioW, ioH)) {
            LOG_ERROR("Failed to create shared IOSurface");
            return 1;
        }
    }

    if (!CreateSession(app, renderer)) {
        LOG_ERROR("Failed to create session");
        return 1;
    }

    if (!CreateSpaces(app)) {
        LOG_ERROR("Failed to create spaces");
        return 1;
    }

    if (!CreateSwapchain(app)) {
        LOG_ERROR("Failed to create swapchain");
        return 1;
    }

    // HUD window-space layer swapchain (XR_EXT_window_space_layer).
    XrHudSwapchain hudSwapchain;
    HudRendererMacOS hudRenderer = {};
    std::vector<GLuint> hudSwapchainImages;
    bool hudReady = false;
    if (CreateHudSwapchain(app.session, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT, hudSwapchain)) {
        std::vector<XrSwapchainImageOpenGLKHR> hudGL(hudSwapchain.imageCount,
            {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
        if (XR_SUCCEEDED(xrEnumerateSwapchainImages(hudSwapchain.swapchain,
                hudSwapchain.imageCount, &hudSwapchain.imageCount,
                (XrSwapchainImageBaseHeader*)hudGL.data()))) {
            hudSwapchainImages.resize(hudSwapchain.imageCount);
            for (uint32_t i = 0; i < hudSwapchain.imageCount; i++) {
                hudSwapchainImages[i] = hudGL[i].image;
            }
            if (InitializeHudRenderer(hudRenderer, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT)) {
                hudReady = true;
                LOG_INFO("HUD window-space layer ready (%ux%u, %u images, format %lld)",
                    HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT, hudSwapchain.imageCount,
                    (long long)hudSwapchain.format);
            } else {
                LOG_WARN("HudRendererMacOS init failed - HUD disabled");
            }
        } else {
            LOG_WARN("Failed to enumerate HUD swapchain images - HUD disabled");
        }
    } else {
        LOG_WARN("Failed to create HUD swapchain - HUD disabled");
    }

    // Initial rendering mode is sourced from the runtime via v13 `isActive`
    // (set during xrEnumerateDisplayRenderingModesEXT above). Fallback is
    // mode 1 (default of app.currentModeIndex). No env-var override here —
    // SIM_DISPLAY_OUTPUT is a runtime-side construct.

    g_input.viewParams.virtualDisplayHeight = 0.24f;
    g_input.initialVirtualDisplayHeight = g_input.viewParams.virtualDisplayHeight;
    g_input.nominalViewerZ = app.nominalViewerZ;

    LOG_INFO("Entering main loop... (ESC to quit, drag to rotate, WASD to move, Space to reset)");
    LOG_INFO("Controls: WASD/QE=Move, Drag=Look, Scroll=Scale, Space=Reset, V=Mode, Shift+Tab=HUD, ESC=Quit");
    LOG_INFO("Zones:    M=wish mode (AUTO/Tier-2 rects), O=zone B overlap toggle, DXR_ZONES_VALIDATE=1");

    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !app.exitRequested) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        PumpMacOSEvents();
        PollEvents(app);

        // Handle rendering mode requests (V=cycle next, 0-8=jump absolute).
        // Single source of truth: the runtime owns the current mode. Keypresses
        // are REQUESTS — we call xrRequestDisplayRenderingModeEXT and let the
        // XrEventDataRenderingModeChangedEXT event update app.currentModeIndex.
        // Render paths and HUD read app.currentModeIndex directly.
        if (g_input.cycleRenderingModeRequested) {
            g_input.cycleRenderingModeRequested = false;
            if (app.pfnRequestDisplayRenderingModeEXT && app.session != XR_NULL_HANDLE &&
                app.renderingModeCount > 0) {
                uint32_t next = (app.currentModeIndex + 1) % app.renderingModeCount;
                app.pfnRequestDisplayRenderingModeEXT(app.session, next);
            }
        }
        if (g_input.absoluteRenderingModeRequested >= 0) {
            uint32_t target = (uint32_t)g_input.absoluteRenderingModeRequested;
            g_input.absoluteRenderingModeRequested = -1;
            if (app.pfnRequestDisplayRenderingModeEXT && app.session != XR_NULL_HANDLE &&
                target < app.renderingModeCount) {
                app.pfnRequestDisplayRenderingModeEXT(app.session, target);
            }
        }

        UpdateCameraMovement(g_input, dt, app.displayHeightM);

        if (!app.sessionRunning) {
            usleep(10000);
            continue;
        }

        // Update animation
        renderer.cubeRotation += dt * g_spinSpeed; // agent-settable via cube-zones-texture-gl__set_spin

        // #439 cases 2/3/4 activation: create + fill the panel swapchain(s)
        // (+ the explicit Tier-2 island mask for case 2) a few frames in,
        // once the session is running and window dims are settled.
        if (g_l2dPanel && !g_l2dActive && g_frameCounter >= g_l2dActivationFrame) {
            static bool attempted = false;
            if (!attempted) {
                attempted = true;
                uint32_t winW = g_windowW;
                uint32_t winH = g_windowH;
                uint32_t pw = winW * 3 / 8;
                uint32_t ph = winH * 5 / 16;
                // #491 validation aid: DXR_LOCAL2D_OVERCUBE centers the panel
                // over the cube + uses the diagonal-stripes variant (clearest
                // read of translucent-2D-over-3D glass).
                const char *oc = getenv("DXR_LOCAL2D_OVERCUBE");
                bool overCube = (oc && *oc == '1');
                int p1variant = overCube ? 1 : 0;
                if (overCube) {
                    g_panel1Rect.offset = {(int32_t)(winW / 2 - pw / 2), (int32_t)(winH / 2 - ph / 2)};
                } else {
                    g_panel1Rect.offset = {(int32_t)(winW / 16), (int32_t)(winH * 9 / 16)};
                }
                g_panel1Rect.extent = {(int32_t)pw, (int32_t)ph};
                bool ok = CreateAndFillL2DPanel(app, pw, ph, p1variant, g_panel1);

                if (ok && g_l2dPanel2) {
                    // Overlaps panel 1's top-right quadrant — list-order
                    // stacking check (panel 2 is later in the list = on top).
                    g_panel2Rect.offset = {g_panel1Rect.offset.x + (int32_t)(pw / 2),
                                           g_panel1Rect.offset.y - (int32_t)(ph / 4)};
                    g_panel2Rect.extent = {(int32_t)pw, (int32_t)ph};
                    ok = CreateAndFillL2DPanel(app, pw, ph, 1, g_panel2);
                }

                if (ok && g_l2dMask && app.hasLocal3DZoneExt &&
                    app.pfnCreateLocal3DZoneMaskEXT != nullptr &&
                    app.pfnSetLocal3DZoneFromRectsEXT != nullptr &&
                    app.pfnSubmitLocal3DZoneEXT != nullptr) {
                    XrLocal3DZoneMaskCreateInfoEXT mci = {
                        (XrStructureType)XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_EXT};
                    mci.maskWidth = 0; // runtime picks the window backing size
                    mci.maskHeight = 0;
                    ok = XR_SUCCEEDED(app.pfnCreateLocal3DZoneMaskEXT(app.session, &mci,
                                                                      &app.local3DZoneMask));
                    if (ok) {
                        // Two 3D islands: a large center-right one and a
                        // small top-left one. Everything else is 2D — the
                        // panel where it covers, desktop (final.a = 0)
                        // where nothing does.
                        XrRect2Di islands[2];
                        islands[0].offset = {(int32_t)(winW * 7 / 16), (int32_t)(winH / 4)};
                        islands[0].extent = {(int32_t)(winW * 7 / 16), (int32_t)(winH / 2)};
                        islands[1].offset = {(int32_t)(winW / 16), (int32_t)(winH / 16)};
                        islands[1].extent = {(int32_t)(winW / 4), (int32_t)(winH / 4)};
                        ok = XR_SUCCEEDED(app.pfnSetLocal3DZoneFromRectsEXT(app.local3DZoneMask, 2,
                                                                            islands)) &&
                             XR_SUCCEEDED(app.pfnSubmitLocal3DZoneEXT(app.local3DZoneMask));
                    }
                }

                if (ok) {
                    g_l2dActive = true;
                    LOG_INFO("Local2D panels active: panel1 %d,%d %ux%u%s%s",
                             g_panel1Rect.offset.x, g_panel1Rect.offset.y, pw, ph,
                             g_l2dPanel2 ? " + panel2 (unpremultiplied, overlapping)" : "",
                             g_l2dMask ? " + explicit Tier-2 island mask" : " (implicit mask)");
                } else {
                    LOG_ERROR("Local2D panel activation failed");
                }
            }
        }

        // Wait frame
        XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState = {XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(app.session, &waitInfo, &frameState))) {
            continue;
        }

        XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
        if (XR_FAILED(xrBeginFrame(app.session, &beginInfo))) {
            continue;
        }

        // ---- zones path (XR_EXT_display_zones, ADR-027) --------------------
        if (g_hasDisplayZonesExt && !g_zonesActive && !g_zonesAttempted &&
            g_frameCounter >= kZonesActivationFrame) {
            TryActivateZones(app);
        }
        if (g_zonesActive && g_hasDisplayZonesExt) {
            HandleZoneKeys(app);
            if (frameState.shouldRender) {
                RenderZonesFrame(app, renderer, frameState);
            } else {
                XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                endInfo.displayTime = frameState.predictedDisplayTime;
                endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                endInfo.layerCount = 0;
                endInfo.layers = nullptr;
                xrEndFrame(app.session, &endInfo);
            }
            // TEXTURE MODE: the runtime composited the full-window multi-zone
            // super-atlas into our shared IOSurface — present it ourselves and
            // run the readback verification.
            PresentAndMaybeDump(renderer);
            g_frameCounter++;
            g_avgFrameTime = g_avgFrameTime * 0.95 + dt * 0.05;
            continue;
        }

        // ---- fallback: plain single-projection cube (extension missing,
        // zones activation failed, or pre-activation frames) ------------------
        std::vector<XrView> views(app.configViews.size(), {XR_TYPE_VIEW});
        XrViewState viewState = {XR_TYPE_VIEW_STATE};
        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.viewConfigurationType = app.viewConfigType;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = app.localSpace;

        // XR_EXT_view_rig (#396 W7): drive the runtime rig matching the app's
        // current mode (C selects the rig) with the app's tunables — the
        // runtime owns the window resolve and the Kooima math, and returns
        // render-ready XrView{pose, fov}. Per-locate semantics: chain the rig
        // on every consume locate. The raw result struct feeds the HUD's
        // display-space eye readout (under the rig, views[] carries world
        // eyes, not raw display eyes).
        const bool useRig =
            app.hasViewRigExt && app.displayWidthM > 0 && app.displayHeightM > 0;
        const bool rigCamera = useRig && g_input.cameraMode;
        XrCameraRigEXT cameraRig = {XR_TYPE_CAMERA_RIG_EXT};
        XrDisplayRigEXT displayRig = {XR_TYPE_DISPLAY_RIG_EXT};
        XrViewDisplayRawEXT viewRigRaw = {XR_TYPE_VIEW_DISPLAY_RAW_EXT};
        XrPosef rigPose = {{0, 0, 0, 1}, {0, 0, 0}};
        if (useRig) {
            quat_from_yaw_pitch(g_input.yaw, g_input.pitch, &rigPose.orientation);
            rigPose.position = {g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ};
            if (rigCamera) {
                cameraRig.pose = rigPose;
                cameraRig.ipdFactor = g_input.viewParams.ipdFactor;
                cameraRig.parallaxFactor = g_input.viewParams.parallaxFactor;
                cameraRig.convergenceDiopters = g_input.viewParams.invConvergenceDistance;
                cameraRig.verticalFov =
                    2.0f * atanf(CAMERA_HALF_TAN_VFOV / g_input.viewParams.zoomFactor);
                // metersToVirtual carries the eye scale the C-toggle converter
                // derived from the display rig, so the camera rig reproduces the
                // display rig exactly.
                cameraRig.metersToVirtual = g_input.viewParams.cameraM2v;
                locateInfo.next = &cameraRig;
            } else {
                displayRig.pose = rigPose;
                displayRig.virtualDisplayHeight =
                    g_input.viewParams.virtualDisplayHeight / g_input.viewParams.scaleFactor;
                displayRig.ipdFactor = g_input.viewParams.ipdFactor;
                displayRig.parallaxFactor = g_input.viewParams.parallaxFactor;
                displayRig.perspectiveFactor = g_input.viewParams.perspectiveFactor;
                locateInfo.next = &displayRig;
            }
            viewState.next = &viewRigRaw;
        }

        uint32_t viewCount = 0;
        xrLocateViews(app.session, &locateInfo, &viewState, (uint32_t)views.size(), &viewCount, views.data());

        // Capture the runtime's resolved CANVAS size (the window client area in
        // meters) — the physical_height_m the Kooima/rig math runs on, which the
        // C-toggle converter must match (windowed → smaller than the display).
        if (useRig && viewRigRaw.canvasSizeMeters.height > 0.0f) {
            g_input.canvasWidthM = viewRigRaw.canvasSizeMeters.width;
            g_input.canvasHeightM = viewRigRaw.canvasSizeMeters.height;
        }

        // XR_EXT_view_rig raw-channel verification (#396 W7): one-shot proof
        // the raw channel reports the DP's full per-view set.
        if (useRig) {
            static int rawLogged = 0;
            if (rawLogged < 3) {
                rawLogged++;
                LOG_INFO("view-rig RAW: eyeCountOutput=%u viewCount=%u isTracking=%d",
                         viewRigRaw.eyeCountOutput, viewCount, (int)viewRigRaw.isTracking);
                for (uint32_t i = 0; i < viewRigRaw.eyeCountOutput && i < 8; i++) {
                    LOG_INFO("  rawEyes[%u]=(%.4f,%.4f,%.4f)", i, viewRigRaw.rawEyes[i].x,
                             viewRigRaw.rawEyes[i].y, viewRigRaw.rawEyes[i].z);
                }
            }
        }

        // Acquire swapchain image
        XrSwapchainImageAcquireInfo acqInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t imageIndex = 0;
        if (XR_FAILED(xrAcquireSwapchainImage(app.swapchain.swapchain, &acqInfo, &imageIndex))) {
            LOG_WARN("xrAcquireSwapchainImage failed");
            XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = frameState.predictedDisplayTime;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            xrEndFrame(app.session, &endInfo);
            continue;
        }

        XrSwapchainImageWaitInfo waitImgInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitImgInfo.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(app.swapchain.swapchain, &waitImgInfo);

        bool rendered = false;
        bool display3D = (app.currentModeIndex < app.renderingModeCount)
            ? app.renderingModeDisplay3D[app.currentModeIndex] : true;

        // Get N-view mode info from enumerated rendering modes
        uint32_t modeViewCount = (app.currentModeIndex < app.renderingModeCount)
            ? app.renderingModeViewCounts[app.currentModeIndex] : 2;
        uint32_t tileColumns = (app.currentModeIndex < app.renderingModeCount)
            ? app.renderingModeTileColumns[app.currentModeIndex] : 2;
        uint32_t tileRows = (app.currentModeIndex < app.renderingModeCount)
            ? app.renderingModeTileRows[app.currentModeIndex] : 1;
        int eyeCount = display3D ? (int)modeViewCount : 1;

        // Dynamic arrays for N-view rendering
        std::vector<XrCompositionLayerProjectionView> projViews(eyeCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        // Render
        if (frameState.shouldRender && viewCount >= 1) {
            // Save display-space eye positions for the HUD. Under the rig,
            // views[] carries render-ready world eyes — the raw channel
            // (XrViewDisplayRawEXT) keeps the HUD readout in display space.
            app.eyeCount = modeViewCount;
            if (useRig && viewRigRaw.eyeCountOutput > 0) {
                for (uint32_t v = 0; v < viewRigRaw.eyeCountOutput && v < 8; v++) {
                    app.eyePositions[v][0] = viewRigRaw.rawEyes[v].x;
                    app.eyePositions[v][1] = viewRigRaw.rawEyes[v].y;
                    app.eyePositions[v][2] = viewRigRaw.rawEyes[v].z;
                }
            } else {
                for (uint32_t v = 0; v < viewCount && v < 8; v++) {
                    app.eyePositions[v][0] = views[v].pose.position.x;
                    app.eyePositions[v][1] = views[v].pose.position.y;
                    app.eyePositions[v][2] = views[v].pose.position.z;
                }
            }

            // Compute render dims from enumerated mode info
            float scaleX = (app.currentModeIndex < app.renderingModeCount)
                ? app.renderingModeScaleX[app.currentModeIndex] : 0.5f;
            float scaleY = (app.currentModeIndex < app.renderingModeCount)
                ? app.renderingModeScaleY[app.currentModeIndex] : 0.5f;
            app.recommendedViewScaleX = scaleX;
            app.recommendedViewScaleY = scaleY;
            uint32_t maxTileW = tileColumns > 0 ? app.swapchain.width / tileColumns : app.swapchain.width;
            uint32_t maxTileH = tileRows > 0 ? app.swapchain.height / tileRows : app.swapchain.height;
            uint32_t renderW, renderH;
            if (!display3D) {
                renderW = g_windowW;
                renderH = g_windowH;
                if (renderW > app.swapchain.width) renderW = app.swapchain.width;
                if (renderH > app.swapchain.height) renderH = app.swapchain.height;
            } else {
                renderW = (uint32_t)(g_windowW * scaleX);
                renderH = (uint32_t)(g_windowH * scaleY);
                if (renderW > maxTileW) renderW = maxTileW;
                if (renderH > maxTileH) renderH = maxTileH;
            }
            g_renderW = renderW;
            g_renderH = renderH;

            // --- Consume the runtime's render-ready XrView{pose, fov} ---
            // (#396 W7) Only clip policy stays app-side, by design (fov is
            // clip-independent). Camera rig: same absolute clip as the old
            // app-side camera path. Display rig: ZDP-anchored clip (near =
            // ez - vH, far = ez + 1000·vH; ez = rig-local z of the view
            // pose). mat4_from_xr_fov is GL [-1,1]-native.
            const float rigVH =
                g_input.viewParams.virtualDisplayHeight / g_input.viewParams.scaleFactor;

            rendered = true;
            std::vector<EyeRenderParams> eyeParams(eyeCount);
            for (int eye = 0; eye < eyeCount; eye++) {
                int vi = eye < (int)viewCount ? eye : 0;
                XrFovf submitFov = views[vi].fov;
                float nearZ = 0.01f, farZ = 100.0f;
                if (useRig && !rigCamera) {
                    float ez = RigLocalEyeZ(rigPose, views[vi].pose.position);
                    nearZ = (ez - rigVH > 0.001f) ? (ez - rigVH) : 0.001f;
                    farZ = ez + 1000.0f * rigVH;
                }
                mat4_view_from_xr_pose(eyeParams[eye].viewMat, views[vi].pose);
                mat4_from_xr_fov(eyeParams[eye].projMat, views[vi].fov, nearZ, farZ);

                // Tile-aware viewport: place each view in the correct tile position
                uint32_t tileX = display3D ? (eye % tileColumns) : 0;
                uint32_t tileY = display3D ? (eye / tileColumns) : 0;
                uint32_t vpX = tileX * renderW;
                uint32_t vpY = tileY * renderH;
                eyeParams[eye].viewportX = vpX;
                eyeParams[eye].viewportY = vpY;
                eyeParams[eye].width = renderW;
                eyeParams[eye].height = renderH;

                projViews[eye].subImage.swapchain = app.swapchain.swapchain;
                projViews[eye].subImage.imageRect.offset = {(int32_t)vpX, (int32_t)vpY};
                projViews[eye].subImage.imageRect.extent = {
                    (int32_t)renderW, (int32_t)renderH};
                projViews[eye].subImage.imageArrayIndex = 0;
                projViews[eye].pose = views[eye < (int)viewCount ? eye : 0].pose;
                projViews[eye].fov = submitFov;
            }

            RenderScene(renderer, app.swapchain.images[imageIndex],
                        app.swapchain.width, app.swapchain.height, eyeParams.data(), eyeCount);

            // 'I' key: snapshot the multi-view atlas via the runtime-owned
            // XR_EXT_atlas_capture (W6 of #396) — the runtime does the readback
            // from its own atlas image, so the app keeps no staging texture.
            // PROJECTION_ONLY = the app's own projection atlas, pre-compose.
            // Skipped for mono (1×1) layouts; the runtime appends
            // "_atlas_<viewCount>_<cols>x<rows>.png".
            if (g_input.captureAtlasRequested) {
                g_input.captureAtlasRequested = false;
                if (app.pfnCaptureAtlasEXT && app.session != XR_NULL_HANDLE) {
                    if (display3D && (tileColumns > 1 || tileRows > 1)) {
                        std::string prefix = dxr_capture::MakeCaptureAtlasPrefix(
                            "cube_zones_texture_gl_macos", tileColumns, tileRows);
                        XrAtlasCaptureInfoEXT info = {XR_TYPE_ATLAS_CAPTURE_INFO_EXT};
                        info.next = nullptr;
                        info.stage = XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_EXT;
                        strncpy(info.pathPrefix, prefix.c_str(),
                                XR_ATLAS_CAPTURE_PATH_MAX_EXT - 1);
                        info.pathPrefix[XR_ATLAS_CAPTURE_PATH_MAX_EXT - 1] = '\0';
                        XrResult cr = app.pfnCaptureAtlasEXT(app.session, &info, nullptr);
                        if (XR_SUCCEEDED(cr)) {
                            LOG_INFO("Atlas capture requested -> %s_atlas_%u_%ux%u.png",
                                     prefix.c_str(), tileColumns * tileRows, tileColumns, tileRows);
                            dxr_capture::TriggerCaptureFlash((__bridge void*)g_glView);
                        } else {
                            LOG_WARN("xrCaptureAtlasEXT failed: 0x%x", (unsigned)cr);
                        }
                    } else {
                        LOG_WARN("Capture skipped: need 3D mode with cols/rows > 1");
                    }
                } else {
                    LOG_WARN("Atlas capture unavailable: XR_EXT_atlas_capture not active");
                }
            }
        }

        // Release swapchain image
        XrSwapchainImageReleaseInfo relInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(app.swapchain.swapchain, &relInfo);

        // Render the HUD into the window-space layer swapchain (when visible).
        bool hudSubmitted = false;
        if (hudReady && g_input.hudVisible && rendered && frameState.shouldRender) {
            uint32_t hudIndex = 0;
            if (AcquireHudSwapchainImage(hudSwapchain, hudIndex)) {
                uint32_t rowPitch = 0;
                const void* pixels = RenderHudAndMap(hudRenderer, &rowPitch,
                    g_hudSessionText, g_hudModeText, g_hudPerfText, g_hudDisplayText,
                    g_hudEyeText, g_hudCameraText, g_hudStereoText, g_hudHelpText);
                if (pixels) {
                    // HUD pixels are BGRA; rowPitch may exceed w*4, so pass the
                    // pixel row length to GL_UNPACK_ROW_LENGTH via the helper.
                    UploadBGRAToSwapchainTex(hudSwapchainImages[hudIndex],
                                             HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT,
                                             pixels, rowPitch / 4);
                    UnmapHud(hudRenderer);
                    hudSubmitted = true;
                }
                ReleaseHudSwapchainImage(hudSwapchain);
            }
        }

        // End frame: projection-only when HUD hidden; projection + window-space HUD otherwise.
        if (hudSubmitted) {
            float hudAR = (float)HUD_PIXEL_WIDTH / (float)HUD_PIXEL_HEIGHT;
            float windowAR = (g_windowW > 0 && g_windowH > 0)
                ? (float)g_windowW / (float)g_windowH : 1.0f;
            float fracW = HUD_WIDTH_FRACTION;
            float fracH = fracW * windowAR / hudAR;
            if (fracH > 1.0f) { fracH = 1.0f; fracW = hudAR / windowAR; }
            SubmitWindowSpaceHudFrame(
                app.session, app.localSpace, frameState.predictedDisplayTime,
                XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                projViews.data(), (uint32_t)eyeCount,
                hudSwapchain, 0.0f, 0.0f, fracW, fracH, 0.0f);
        } else {
            XrCompositionLayerProjection projLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projLayer.space = app.localSpace;
            projLayer.viewCount = (uint32_t)eyeCount;
            projLayer.views = projViews.data();

            // #439 cases 2/3/4: Local2D panel layers ride the normal layer
            // list after the projection layer (list order = stacking order).
            XrCompositionLayerLocal2DEXT panel1Layer = {
                (XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT};
            XrCompositionLayerLocal2DEXT panel2Layer = {
                (XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT};
            const XrCompositionLayerBaseHeader *layers[3] = {
                (XrCompositionLayerBaseHeader *)&projLayer, nullptr, nullptr};
            uint32_t layerCount = (rendered && frameState.shouldRender) ? 1 : 0;
            if (layerCount > 0 && g_l2dActive && g_panel1.swapchain != XR_NULL_HANDLE) {
                panel1Layer.layerFlags = 0; // premultiplied bytes
                panel1Layer.subImage.swapchain = g_panel1.swapchain;
                panel1Layer.subImage.imageRect.offset = {0, 0};
                panel1Layer.subImage.imageRect.extent = {(int32_t)g_panel1.w, (int32_t)g_panel1.h};
                panel1Layer.subImage.imageArrayIndex = 0;
                panel1Layer.rect = g_panel1Rect;
                layers[layerCount++] = (XrCompositionLayerBaseHeader *)&panel1Layer;

                if (g_l2dPanel2 && g_panel2.swapchain != XR_NULL_HANDLE) {
                    panel2Layer.layerFlags = XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                    panel2Layer.subImage.swapchain = g_panel2.swapchain;
                    panel2Layer.subImage.imageRect.offset = {0, 0};
                    panel2Layer.subImage.imageRect.extent = {(int32_t)g_panel2.w, (int32_t)g_panel2.h};
                    panel2Layer.subImage.imageArrayIndex = 0;
                    panel2Layer.rect = g_panel2Rect;
                    layers[layerCount++] = (XrCompositionLayerBaseHeader *)&panel2Layer;
                }
            }

            XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = frameState.predictedDisplayTime;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            endInfo.layerCount = layerCount;
            endInfo.layers = layers;

            xrEndFrame(app.session, &endInfo);
        }

        // TEXTURE MODE: present the IOSurface ourselves (fallback /
        // pre-activation path — projection-only composite) + verification.
        PresentAndMaybeDump(renderer);

        g_frameCounter++;

        // FPS tracking
        g_avgFrameTime = g_avgFrameTime * 0.95 + dt * 0.05;

        // Refresh cached HUD section strings (throttled). RenderHudAndMap is
        // called every frame the HUD is visible (above), but the wstring
        // content only changes a few times per second.
        g_hudUpdateTimer += dt;
        if (g_hudUpdateTimer >= 0.5f) {
            g_hudUpdateTimer = 0.0f;

            auto utf8ToW = [](const char* s) -> std::wstring {
                NSString* ns = [NSString stringWithUTF8String:(s ? s : "")];
                NSData* d = [ns dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
                size_t n = d.length / sizeof(wchar_t);
                std::wstring w(n, L'\0');
                if (n) memcpy((void*)w.data(), d.bytes, d.length);
                return w;
            };

            const char* sessionStateNames[] = {
                "UNKNOWN", "IDLE", "READY", "SYNCHRONIZED",
                "VISIBLE", "FOCUSED", "STOPPING", "LOSS_PENDING", "EXITING"};
            int stateIdx = (int)app.sessionState;
            const char* sessionStateName = (stateIdx >= 0 && stateIdx < 9)
                ? sessionStateNames[stateIdx] : "INVALID";

            char buf[512];
            snprintf(buf, sizeof(buf), "%s\nSession: %s",
                app.systemName, sessionStateName);
            g_hudSessionText = utf8ToW(buf);

            const char* outputModeName = (app.currentModeIndex < app.renderingModeCount)
                ? app.renderingModeNames[app.currentModeIndex] : "?";
            bool isReq = (app.currentModeIndex < app.renderingModeCount)
                ? app.renderingModeIsRequestable[app.currentModeIndex] : true;
            const char* lockSuffix = isReq ? "" : " [locked by workspace]";
            const char* kooimaMode = g_input.cameraMode
                ? "Camera-Centric [C=Toggle]" : "Display-Centric [C=Toggle]";
            snprintf(buf, sizeof(buf),
                "XR_EXT_cocoa_window_binding: %s (OpenGL)\nMode: %s (%s)%s\nKooima: %s",
                app.hasCocoaWindowBinding ? "ACTIVE" : "NOT AVAILABLE",
                outputModeName, display3D ? "3D" : "2D", lockSuffix, kooimaMode);
            g_hudModeText = utf8ToW(buf);

            double fps = (g_avgFrameTime > 0) ? 1.0 / g_avgFrameTime : 0;
            snprintf(buf, sizeof(buf), "FPS: %.0f  (%.1f ms)\nRender: %ux%u  Window: %ux%u",
                fps, g_avgFrameTime * 1000.0,
                g_renderW, g_renderH, g_windowW, g_windowH);
            g_hudPerfText = utf8ToW(buf);

            snprintf(buf, sizeof(buf),
                "Display: %.3f x %.3f m\nScale: %.2f x %.2f\nNominal: (%.3f, %.3f, %.3f)",
                app.displayWidthM, app.displayHeightM,
                app.recommendedViewScaleX, app.recommendedViewScaleY,
                app.nominalViewerX, app.nominalViewerY, app.nominalViewerZ);
            g_hudDisplayText = utf8ToW(buf);

            std::string eyeLines;
            for (uint32_t e = 0; e < app.eyeCount && e < 8; e++) {
                char line[128];
                snprintf(line, sizeof(line), "Eye[%u]: (%.3f, %.3f, %.3f)%s",
                    e, app.eyePositions[e][0], app.eyePositions[e][1], app.eyePositions[e][2],
                    (e + 1 < app.eyeCount && e + 1 < 8) ? "\n" : "");
                eyeLines += line;
            }
            g_hudEyeText = utf8ToW(eyeLines.c_str());

            const char* poseLabel = g_input.cameraMode ? "Virtual Camera" : "Virtual Display";
            snprintf(buf, sizeof(buf), "%s: (%.2f, %.2f, %.2f)",
                poseLabel, g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ);
            g_hudCameraText = utf8ToW(buf);

            const char* param1Label = g_input.cameraMode ? "Conv" : "Persp";
            const char* param2Label = g_input.cameraMode ? "Zoom" : "Scale";
            float param1Val = g_input.cameraMode
                ? g_input.viewParams.invConvergenceDistance : g_input.viewParams.perspectiveFactor;
            float param2Val = g_input.cameraMode
                ? g_input.viewParams.zoomFactor : g_input.viewParams.scaleFactor;
            char valueLine[96];
            if (g_input.cameraMode) {
                float tanHFOV = CAMERA_HALF_TAN_VFOV / g_input.viewParams.zoomFactor;
                snprintf(valueLine, sizeof(valueLine), "tanHFOV: %.3f", tanHFOV);
            } else {
                float m2v = (g_input.viewParams.virtualDisplayHeight > 0.0f && app.displayHeightM > 0.0f)
                    ? g_input.viewParams.virtualDisplayHeight / app.displayHeightM : 1.0f;
                snprintf(valueLine, sizeof(valueLine), "vHeight: %.3f  m2v: %.3f",
                    g_input.viewParams.virtualDisplayHeight, m2v);
            }
            snprintf(buf, sizeof(buf), "IPD: %.2f  Parallax: %.2f\n%s: %.2f  %s: %.2f\n%s",
                g_input.viewParams.ipdFactor, g_input.viewParams.parallaxFactor,
                param1Label, param1Val, param2Label, param2Val, valueLine);
            g_hudStereoText = utf8ToW(buf);

            const char* scrollHint = g_input.cameraMode ? "Scroll=Zoom" : "Scroll=Scale";
            const char* perspHint = g_input.cameraMode ? "Opt=Conv" : "Opt=Persp";
            char outputHint[32] = "";
            if (app.renderingModeCount > 1) {
                snprintf(outputHint, sizeof(outputHint), "  0-%u=Mode", app.renderingModeCount - 1);
            }
            snprintf(buf, sizeof(buf),
                "WASD/QE=Move  Drag=Look  Space=Reset\n"
                "%s  Shift=IPD  Ctrl=Parallax  %s\n"
                "V=Mode%s  Shift+Tab=HUD  ESC=Quit",
                scrollHint, perspHint, outputHint);
            g_hudHelpText = utf8ToW(buf);
        }
    }

    LOG_INFO("Shutting down...");

    if (hudReady) {
        CleanupHudRenderer(hudRenderer);
    }
    if (hudSwapchain.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(hudSwapchain.swapchain);
    }
    if (app.local3DZoneMask != XR_NULL_HANDLE && app.pfnDestroyLocal3DZoneMaskEXT != nullptr) {
        app.pfnDestroyLocal3DZoneMaskEXT(app.local3DZoneMask);
    }
    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        if (g_zonesArr[zi].depthRBO) glDeleteRenderbuffers(1, &g_zonesArr[zi].depthRBO);
        if (g_zonesArr[zi].fbo) glDeleteFramebuffers(1, &g_zonesArr[zi].fbo);
        if (g_zonesArr[zi].swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(g_zonesArr[zi].swapchain);
        }
    }
    if (g_strip.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_strip.swapchain);
    }
    if (g_panel1.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_panel1.swapchain);
    }
    if (g_panel2.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_panel2.swapchain);
    }
    if (app.swapchain.swapchain)
        xrDestroySwapchain(app.swapchain.swapchain);
    if (app.localSpace)
        xrDestroySpace(app.localSpace);
    if (app.viewSpace)
        xrDestroySpace(app.viewSpace);
    if (app.session)
        xrDestroySession(app.session);
    if (app.instance)
        xrDestroyInstance(app.instance);

    // Texture-mode shared IOSurface.
    if (g_presentTex != 0) {
        glDeleteTextures(1, &g_presentTex);
        g_presentTex = 0;
    }
    if (g_ioSurface != NULL) {
        CFRelease(g_ioSurface);
        g_ioSurface = NULL;
    }

    LOG_INFO("Clean shutdown complete");
    return 0;
}
