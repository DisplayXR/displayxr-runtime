// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Metal OpenXR spinning cube — texture mode (real view + shared IOSurface)
 *
 * Demonstrates XR_EXT_cocoa_window_binding in Texture mode, matching the
 * Windows texture apps (real HWND + shared HANDLE): the app passes BOTH its
 * real NSView (viewHandle — used by the display processor for screen-space
 * position tracking / phase alignment) AND a shared IOSurface
 * (sharedIOSurface — the runtime weaves into its canvas sub-rect, declared
 * via xrSetSharedTextureOutputRectEXT). The app owns presentation: it blits
 * the IOSurface back into its own CAMetalLayer.
 *
 * The app also registers a window-sized 2D surround IOSurface via
 * xrSetSharedTextureSurround2DEXT (spec v6; window-clamped per #464) — the
 * runtime strip-blits its non-canvas pixels into the shared IOSurface each
 * frame, and the app presents the full window region (3D canvas + 2D
 * surround).
 *
 * Key difference from cube_handle_metal_macos: the runtime does not present
 * to the app's CAMetalLayer. The IOSurface acts as a shared render target,
 * and the app composites the result into its own rendering pipeline.
 *
 * Features:
 * - Real-view + shared-IOSurface binding (zero-copy Metal texture sharing)
 * - 2D surround (checkerboard pattern) around a center-50% 3D canvas
 * - App-owned window with toolbar and status bar UI
 * - Mouse drag camera rotation, scroll zoom, WASD movement
 * - Metal rendering (no Vulkan/MoltenVK dependency)
 * - ESC to quit, Space to reset view
 */

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>

#define XR_USE_GRAPHICS_API_METAL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_cocoa_window_binding.h>
#include <openxr/XR_EXT_local_3d_zone.h>

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
#include "view_params.h"
#include "xr_window_space_hud.h"
#include "hud_renderer_macos.h"
#include <openxr/XR_EXT_display_info.h>
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
    m[10] = -farZ / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -(farZ * nearZ) / (farZ - nearZ);
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
// Quaternion helpers
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
// Metal Shading Language shaders (embedded strings)
// ============================================================================

static const char *g_metalShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

// --- Cube shaders ---

struct CubeVertexIn {
    float3 pos      [[attribute(0)]];
    float4 color    [[attribute(1)]];
    float2 uv       [[attribute(2)]];
    float3 normal   [[attribute(3)]];
    float3 tangent  [[attribute(4)]];
};

struct CubeVertexOut {
    float4 position [[position]];
    float2 uv;
    float3 worldNormal;
    float3 worldTangent;
};

struct CubeUniforms {
    float4x4 mvp;
    float4x4 model;
};

vertex CubeVertexOut cube_vertex(CubeVertexIn in [[stage_in]],
                                  constant CubeUniforms &uniforms [[buffer(1)]])
{
    CubeVertexOut out;
    out.position = uniforms.mvp * float4(in.pos, 1.0);
    out.uv = in.uv;
    out.worldNormal = (uniforms.model * float4(in.normal, 0.0)).xyz;
    out.worldTangent = (uniforms.model * float4(in.tangent, 0.0)).xyz;
    return out;
}

fragment float4 cube_fragment(CubeVertexOut in [[stage_in]],
                               texture2d<float> basecolorTex [[texture(0)]],
                               texture2d<float> normalTex    [[texture(1)]],
                               texture2d<float> aoTex        [[texture(2)]],
                               sampler texSampler            [[sampler(0)]])
{
    float4 baseColor = basecolorTex.sample(texSampler, in.uv);
    float3 normalMap = normalTex.sample(texSampler, in.uv).xyz * 2.0 - 1.0;
    float ao = aoTex.sample(texSampler, in.uv).r;

    float3 N = normalize(in.worldNormal);
    float3 T = normalize(in.worldTangent);
    float3 B = cross(N, T);
    float3x3 TBN = float3x3(T, B, N);
    float3 normal = normalize(TBN * normalMap);

    float3 lightDir = normalize(float3(0.3, 0.5, 1.0));
    float diffuse = max(dot(normal, lightDir), 0.0) * 0.8 * ao;
    float ambient = 0.3 + 0.15 * ao;

    return float4(baseColor.rgb * (diffuse + ambient), 1.0);
}

// --- Grid shaders ---

struct GridVertexIn {
    float3 pos [[attribute(0)]];
};

struct GridVertexOut {
    float4 position [[position]];
};

struct GridUniforms {
    float4x4 mvp;
    float4 color;
};

vertex GridVertexOut grid_vertex(GridVertexIn in [[stage_in]],
                                 constant GridUniforms &uniforms [[buffer(1)]])
{
    GridVertexOut out;
    out.position = uniforms.mvp * float4(in.pos, 1.0);
    return out;
}

fragment float4 grid_fragment(GridVertexOut in [[stage_in]],
                               constant GridUniforms &uniforms [[buffer(1)]])
{
    return uniforms.color;
}

// --- Blit shader (fullscreen triangle sampling IOSurface texture) ---

struct BlitParams {
    float2 uv_scale;  // (canvasW/surfaceW, canvasH/surfaceH)
    float2 uv_offset; // (canvasX/surfaceW, canvasY/surfaceH) — per ADR-010
};

struct BlitVertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex BlitVertexOut blit_vertex(uint vid [[vertex_id]],
                                 constant BlitParams &params [[buffer(0)]])
{
    BlitVertexOut out;
    out.uv = float2((vid << 1) & 2, vid & 2);
    out.position = float4(out.uv * 2.0 - 1.0, 0.0, 1.0);
    out.uv.y = 1.0 - out.uv.y; // Flip Y for Metal
    out.uv = params.uv_offset + out.uv * params.uv_scale;  // Sample canvas sub-rect of IOSurface
    return out;
}

fragment float4 blit_fragment(BlitVertexOut in [[stage_in]],
                                texture2d<float> tex [[texture(0)]],
                                sampler samp [[sampler(0)]])
{
    return tex.sample(samp, in.uv);
}
)MSL";

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

struct CubeUniforms {
    float mvp[16];
    float model[16];
};

struct GridUniforms {
    float mvp[16];
    float color[4];
};

struct EyeRenderParams {
    uint32_t viewportX, viewportY, width, height;
    float viewMat[16];
    float projMat[16];
};

// ============================================================================
// Cube geometry (24 verts, 36 indices -- 6 faces with unique normals)
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
// Metal renderer
// ============================================================================

struct MetalRenderer {
    id<MTLDevice>              device;
    id<MTLCommandQueue>        commandQueue;
    id<MTLLibrary>             shaderLibrary;

    id<MTLRenderPipelineState> cubePipeline;
    id<MTLRenderPipelineState> gridPipeline;
    id<MTLRenderPipelineState> blitPipeline;
    id<MTLDepthStencilState>   depthState;

    id<MTLBuffer>              cubeVertexBuffer;
    id<MTLBuffer>              cubeIndexBuffer;
    id<MTLBuffer>              gridVertexBuffer;
    int                        gridVertexCount;

    id<MTLTexture>             textures[3]; // basecolor, normal, AO
    id<MTLSamplerState>        sampler;
    bool                       texturesLoaded;

    id<MTLTexture>             depthTexture;
    uint32_t                   depthWidth, depthHeight;

    float                      cubeRotation;
};

// ============================================================================
// Texture loading
// ============================================================================

static id<MTLTexture> LoadTextureFromFile(id<MTLDevice> device,
                                           id<MTLCommandQueue> queue,
                                           const char *path,
                                           uint8_t fallbackR, uint8_t fallbackG,
                                           uint8_t fallbackB)
{
    (void)queue;
    int w, h, channels;
    stbi_uc *pixels = stbi_load(path, &w, &h, &channels, 4);

    if (!pixels) {
        LOG_WARN("Texture not found: %s (using fallback)", path);
        w = h = 1;
        uint8_t fallback[4] = {fallbackR, fallbackG, fallbackB, 255};

        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                        width:1
                                                                                       height:1
                                                                                    mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
        [tex replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:fallback bytesPerRow:4];
        return tex;
    }

    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                    width:w
                                                                                   height:h
                                                                                mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
    [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:pixels bytesPerRow:w * 4];

    stbi_image_free(pixels);
    LOG_INFO("Loaded texture: %s (%dx%d)", path, w, h);
    return tex;
}

// ============================================================================
// Renderer setup
// ============================================================================

static bool InitRenderer(MetalRenderer &r)
{
    r.device = MTLCreateSystemDefaultDevice();
    if (!r.device) {
        LOG_ERROR("No Metal device available");
        return false;
    }
    LOG_INFO("Metal device: %s", r.device.name.UTF8String);

    r.commandQueue = [r.device newCommandQueue];

    NSError *error = nil;
    r.shaderLibrary = [r.device newLibraryWithSource:[NSString stringWithUTF8String:g_metalShaderSource]
                                             options:nil
                                               error:&error];
    if (!r.shaderLibrary) {
        LOG_ERROR("Shader compilation failed: %s", error.localizedDescription.UTF8String);
        return false;
    }

    // Cube vertex layout
    MTLVertexDescriptor *cubeVertDesc = [[MTLVertexDescriptor alloc] init];
    cubeVertDesc.attributes[0].format = MTLVertexFormatFloat3;
    cubeVertDesc.attributes[0].offset = offsetof(CubeVertex, pos);
    cubeVertDesc.attributes[0].bufferIndex = 0;
    cubeVertDesc.attributes[1].format = MTLVertexFormatFloat4;
    cubeVertDesc.attributes[1].offset = offsetof(CubeVertex, color);
    cubeVertDesc.attributes[1].bufferIndex = 0;
    cubeVertDesc.attributes[2].format = MTLVertexFormatFloat2;
    cubeVertDesc.attributes[2].offset = offsetof(CubeVertex, uv);
    cubeVertDesc.attributes[2].bufferIndex = 0;
    cubeVertDesc.attributes[3].format = MTLVertexFormatFloat3;
    cubeVertDesc.attributes[3].offset = offsetof(CubeVertex, normal);
    cubeVertDesc.attributes[3].bufferIndex = 0;
    cubeVertDesc.attributes[4].format = MTLVertexFormatFloat3;
    cubeVertDesc.attributes[4].offset = offsetof(CubeVertex, tangent);
    cubeVertDesc.attributes[4].bufferIndex = 0;
    cubeVertDesc.layouts[0].stride = sizeof(CubeVertex);

    // Grid vertex layout
    MTLVertexDescriptor *gridVertDesc = [[MTLVertexDescriptor alloc] init];
    gridVertDesc.attributes[0].format = MTLVertexFormatFloat3;
    gridVertDesc.attributes[0].offset = 0;
    gridVertDesc.attributes[0].bufferIndex = 0;
    gridVertDesc.layouts[0].stride = sizeof(GridVertex);

    // Cube pipeline
    {
        MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.vertexFunction = [r.shaderLibrary newFunctionWithName:@"cube_vertex"];
        desc.fragmentFunction = [r.shaderLibrary newFunctionWithName:@"cube_fragment"];
        desc.vertexDescriptor = cubeVertDesc;
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

        r.cubePipeline = [r.device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!r.cubePipeline) {
            LOG_ERROR("Cube pipeline creation failed: %s", error.localizedDescription.UTF8String);
            return false;
        }
    }

    // Grid pipeline
    {
        MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.vertexFunction = [r.shaderLibrary newFunctionWithName:@"grid_vertex"];
        desc.fragmentFunction = [r.shaderLibrary newFunctionWithName:@"grid_fragment"];
        desc.vertexDescriptor = gridVertDesc;
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

        r.gridPipeline = [r.device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!r.gridPipeline) {
            LOG_ERROR("Grid pipeline creation failed: %s", error.localizedDescription.UTF8String);
            return false;
        }
    }

    // Blit pipeline (for blitting IOSurface content to drawable)
    {
        MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.vertexFunction = [r.shaderLibrary newFunctionWithName:@"blit_vertex"];
        desc.fragmentFunction = [r.shaderLibrary newFunctionWithName:@"blit_fragment"];
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

        r.blitPipeline = [r.device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!r.blitPipeline) {
            LOG_ERROR("Blit pipeline creation failed: %s", error.localizedDescription.UTF8String);
            return false;
        }
    }

    // Depth stencil state
    {
        MTLDepthStencilDescriptor *desc = [[MTLDepthStencilDescriptor alloc] init];
        desc.depthCompareFunction = MTLCompareFunctionLess;
        desc.depthWriteEnabled = YES;
        r.depthState = [r.device newDepthStencilStateWithDescriptor:desc];
    }

    // Sampler
    {
        MTLSamplerDescriptor *desc = [[MTLSamplerDescriptor alloc] init];
        desc.minFilter = MTLSamplerMinMagFilterLinear;
        desc.magFilter = MTLSamplerMinMagFilterLinear;
        desc.sAddressMode = MTLSamplerAddressModeRepeat;
        desc.tAddressMode = MTLSamplerAddressModeRepeat;
        r.sampler = [r.device newSamplerStateWithDescriptor:desc];
    }

    // Cube geometry
    r.cubeVertexBuffer = [r.device newBufferWithBytes:g_cubeVertices
                                               length:sizeof(g_cubeVertices)
                                              options:MTLResourceStorageModeShared];
    r.cubeIndexBuffer = [r.device newBufferWithBytes:g_cubeIndices
                                              length:sizeof(g_cubeIndices)
                                             options:MTLResourceStorageModeShared];

    // Grid geometry
    auto gridVerts = BuildGridVertices();
    r.gridVertexCount = (int)gridVerts.size();
    r.gridVertexBuffer = [r.device newBufferWithBytes:gridVerts.data()
                                               length:gridVerts.size() * sizeof(GridVertex)
                                              options:MTLResourceStorageModeShared];

    // Textures
    std::string texDir = GetTextureDir();
    r.textures[0] = LoadTextureFromFile(r.device, r.commandQueue,
                                         (texDir + "Wood_Crate_001_basecolor.jpg").c_str(),
                                         200, 200, 200);
    r.textures[1] = LoadTextureFromFile(r.device, r.commandQueue,
                                         (texDir + "Wood_Crate_001_normal.jpg").c_str(),
                                         128, 128, 255);
    r.textures[2] = LoadTextureFromFile(r.device, r.commandQueue,
                                         (texDir + "Wood_Crate_001_ambientOcclusion.jpg").c_str(),
                                         255, 255, 255);
    r.texturesLoaded = true;

    r.cubeRotation = 0.0f;
    r.depthTexture = nil;
    r.depthWidth = r.depthHeight = 0;

    LOG_INFO("Metal renderer initialized");
    return true;
}

// ============================================================================
// Ensure depth texture matches swapchain size
// ============================================================================

static void EnsureDepthTexture(MetalRenderer &r, uint32_t w, uint32_t h)
{
    if (r.depthTexture && r.depthWidth == w && r.depthHeight == h)
        return;

    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                    width:w
                                                                                   height:h
                                                                                mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModePrivate;
    r.depthTexture = [r.device newTextureWithDescriptor:desc];
    r.depthWidth = w;
    r.depthHeight = h;
}

// ============================================================================
// Render scene into swapchain image
// ============================================================================

static void RenderScene(MetalRenderer &r, id<MTLTexture> target,
                         const EyeRenderParams *eyes, int eyeCount)
{
    EnsureDepthTexture(r, (uint32_t)target.width, (uint32_t)target.height);

    MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.colorAttachments[0].texture = target;
    rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    rpd.colorAttachments[0].clearColor = MTLClearColorMake(0.05, 0.05, 0.08, 1.0);
    rpd.depthAttachment.texture = r.depthTexture;
    rpd.depthAttachment.loadAction = MTLLoadActionClear;
    rpd.depthAttachment.storeAction = MTLStoreActionDontCare;
    rpd.depthAttachment.clearDepth = 1.0;

    id<MTLCommandBuffer> cmdBuf = [r.commandQueue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:rpd];

    [enc setDepthStencilState:r.depthState];

    for (int e = 0; e < eyeCount; e++) {
        const EyeRenderParams &eye = eyes[e];

        MTLViewport vp = {
            (double)eye.viewportX, (double)eye.viewportY,
            (double)eye.width, (double)eye.height,
            0.0, 1.0
        };
        [enc setViewport:vp];

        MTLScissorRect scissor = {eye.viewportX, eye.viewportY, eye.width, eye.height};
        [enc setScissorRect:scissor];

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

            CubeUniforms uniforms;
            mat4_multiply(uniforms.mvp, vp_mat, model);
            memcpy(uniforms.model, model, sizeof(model));

            [enc setRenderPipelineState:r.cubePipeline];
            [enc setVertexBuffer:r.cubeVertexBuffer offset:0 atIndex:0];
            [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
            [enc setFragmentTexture:r.textures[0] atIndex:0];
            [enc setFragmentTexture:r.textures[1] atIndex:1];
            [enc setFragmentTexture:r.textures[2] atIndex:2];
            [enc setFragmentSamplerState:r.sampler atIndex:0];

            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:36
                             indexType:MTLIndexTypeUInt16
                           indexBuffer:r.cubeIndexBuffer
                     indexBufferOffset:0];
        }

        // --- Draw grid ---
        {
            const float gridScale = 0.05f;
            float gridScl[16], gridMvp[16];
            mat4_scaling(gridScl, gridScale);
            mat4_multiply(gridMvp, vp_mat, gridScl);

            GridUniforms uniforms;
            memcpy(uniforms.mvp, gridMvp, sizeof(gridMvp));
            uniforms.color[0] = 0.3f;
            uniforms.color[1] = 0.3f;
            uniforms.color[2] = 0.35f;
            uniforms.color[3] = 1.0f;

            [enc setRenderPipelineState:r.gridPipeline];
            [enc setVertexBuffer:r.gridVertexBuffer offset:0 atIndex:0];
            [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
            [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:1];

            [enc drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:r.gridVertexCount];
        }
    }

    [enc endEncoding];

    [cmdBuf commit];
    [cmdBuf waitUntilCompleted];
}

// ============================================================================
// Globals
// ============================================================================

static volatile bool g_running = true;
static NSWindow *g_window = nil;
static NSView *g_metalView = nil;

// IOSurface shared texture
static IOSurfaceRef g_ioSurface = NULL;
static id<MTLTexture> g_ioSurfaceReadTexture = nil;
static uint32_t g_ioSurfaceWidth = 1920;
static uint32_t g_ioSurfaceHeight = 1080;

// 2D surround IOSurface (spec v6 xrSetSharedTextureSurround2DEXT, Cocoa
// form). Window-sized per #464 — reallocated + re-registered on window
// resize, CPU-filled with a static checkerboard+gradient pattern. The
// runtime strip-blits its non-canvas pixels into the shared IOSurface
// each frame.
static IOSurfaceRef g_surroundIOSurface = NULL;
static uint32_t g_surroundWidth = 0;
static uint32_t g_surroundHeight = 0;
static bool g_surroundRegistered = false;

// Input state
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
};
static InputState g_input;
static const float CAMERA_HALF_TAN_VFOV = 0.32491969623f; // tan(18deg) -> 36deg vFOV

// HUD window-space layer (XR_EXT_window_space_layer): replaces the legacy
// NSView overlay so the runtime composes the HUD via the proper extension
// path on macOS. Same constants as the other macOS test apps.
static const uint32_t HUD_PIXEL_WIDTH = 380;
static const uint32_t HUD_PIXEL_HEIGHT = 470;
static const float HUD_WIDTH_FRACTION = 0.20f;

// Performance stats
static double g_avgFrameTime = 0.0;
static float g_hudUpdateTimer = 0.0f;
static uint32_t g_windowW = 1512, g_windowH = 823;  // Full window backing pixels
static uint32_t g_canvasW = 1512, g_canvasH = 823;  // Logical canvas backing pixels (center 50%)

// ============================================================================
// #439 Phase 3 case-1 A/B (DXR_AB_LOCAL2D=1)
// ============================================================================
// B-mode replaces the surround side-channel with the spec-§5 migration
// shape: an explicit Tier-2 single-rect mask (the canvas rect as 3D) + a
// full-window XrCompositionLayerLocal2DEXT carrying the same surround
// pattern. The mask activates at frame N (not startup) so the Q4 view-size
// event fires and the renegotiation flow is exercised end-to-end: the app
// then renders window-sized (the mask supersedes the canvas) and the
// screen-anchored Kooima projection keeps the 3D content on the same
// screen pixels — capture diff vs the legacy surround path ≈ 0.
// Determinism hooks for the A/B capture: DXR_FREEZE=1 stops the cube
// animation, DXR_HUD=0 starts with the HUD hidden.
static bool g_abLocal2D = false;       // B-mode toggle (DXR_AB_LOCAL2D=1)
static bool g_local2DActive = false;   // mask + layer submitted from here on
static bool g_canvasIsWindow = false;  // post view-size event: window-sized rendering
static int g_local2DActivationFrame = 60;
static long g_frameCounter = 0;
static bool g_freezeAnimation = false; // DXR_FREEZE=1
static uint32_t g_renderW = 0, g_renderH = 0;

// Cached HUD section strings, refreshed at HUD throttle rate (~2 Hz).
static std::wstring g_hudSessionText, g_hudModeText, g_hudPerfText;
static std::wstring g_hudDisplayText, g_hudEyeText, g_hudCameraText;
static std::wstring g_hudStereoText, g_hudHelpText;

// UI layout constants
static const float TOOLBAR_HEIGHT = 30.0f;
static const float STATUSBAR_HEIGHT = 30.0f;

static void SignalHandler(int)
{
    g_running = false;
}

// ============================================================================
// Toolbar view (top bar with mode / FPS / info)
// ============================================================================

@interface ToolbarView : NSView
@property (nonatomic, copy) NSString *toolbarText;
@end

@implementation ToolbarView
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) { _toolbarText = @"Real View + Shared IOSurface + 2D Surround"; [self setWantsLayer:YES]; }
    return self;
}
- (BOOL)isOpaque { return YES; }
- (NSView *)hitTest:(NSPoint)point { (void)point; return nil; }
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    [[NSColor colorWithCalibratedRed:0.15 green:0.15 blue:0.2 alpha:1.0] setFill];
    NSRectFill(self.bounds);
    NSFont *font = [NSFont fontWithName:@"Menlo" size:12];
    if (!font) font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    NSDictionary *attrs = @{
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: [NSColor colorWithCalibratedRed:0.8 green:0.9 blue:1.0 alpha:1.0]
    };
    NSRect textRect = NSInsetRect(self.bounds, 10, 4);
    [_toolbarText drawWithRect:textRect
                       options:NSStringDrawingUsesLineFragmentOrigin
                    attributes:attrs context:nil];
}
@end

static ToolbarView *g_toolbarView = nil;

// ============================================================================
// Status bar view (bottom bar with eye pos / display info)
// ============================================================================

@interface StatusBarView : NSView
@property (nonatomic, copy) NSString *statusText;
@end

@implementation StatusBarView
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) { _statusText = @""; [self setWantsLayer:YES]; }
    return self;
}
- (BOOL)isOpaque { return YES; }
- (NSView *)hitTest:(NSPoint)point { (void)point; return nil; }
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    [[NSColor colorWithCalibratedRed:0.12 green:0.12 blue:0.16 alpha:1.0] setFill];
    NSRectFill(self.bounds);
    NSFont *font = [NSFont fontWithName:@"Menlo" size:10];
    if (!font) font = [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular];
    NSDictionary *attrs = @{
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: [NSColor colorWithCalibratedRed:0.7 green:0.7 blue:0.75 alpha:1.0]
    };
    NSRect textRect = NSInsetRect(self.bounds, 10, 4);
    [_statusText drawWithRect:textRect
                      options:NSStringDrawingUsesLineFragmentOrigin
                   attributes:attrs context:nil];
}
@end

static StatusBarView *g_statusBarView = nil;

// ============================================================================
// macOS window creation (CAMetalLayer-backed NSView with UI chrome)
// ============================================================================

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

static bool CreateMacOSWindow(uint32_t width, uint32_t height, id<MTLDevice> device)
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

        [g_window setTitle:@"Metal Cube — Metal Native Compositor (Real View + Shared IOSurface)"];
        [g_window setAcceptsMouseMovedEvents:YES];
        [g_window setReleasedWhenClosed:NO];

        g_windowDelegate = [[AppWindowDelegate alloc] init];
        [g_window setDelegate:g_windowDelegate];

        // Create a container view that holds Metal view + toolbar + status bar.
        // Layer-backed so the toolbar/status-bar overlays reliably composite
        // ABOVE the full-window CAMetalLayer sibling.
        NSView *container = [[NSView alloc] initWithFrame:frame];
        [container setWantsLayer:YES];

        // Metal view — FULL content area (added first = bottom of z-order).
        // The 3D canvas is a logical center sub-rect inside it (see
        // ComputeCanvasRectPx), matching the Windows texture apps where the
        // canvas is a sub-rect of the one full-window client area.
        g_metalView = [[AppMetalView alloc] initWithFrame:[container bounds]];
        [g_metalView setWantsLayer:YES];
        g_metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        // Set Retina scale
        CAMetalLayer *metalLayer = (CAMetalLayer *)[g_metalView layer];
        if (metalLayer) {
            metalLayer.device = device;
            metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            metalLayer.contentsScale = [g_window backingScaleFactor];
        }

        [container addSubview:g_metalView];

        // Toolbar (top, overlays the metal view)
        NSRect toolbarFrame = NSMakeRect(0, height - TOOLBAR_HEIGHT, width, TOOLBAR_HEIGHT);
        g_toolbarView = [[ToolbarView alloc] initWithFrame:toolbarFrame];
        g_toolbarView.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
        [g_toolbarView setWantsLayer:YES];
        [container addSubview:g_toolbarView];

        // Status bar (bottom, overlays the metal view)
        NSRect statusFrame = NSMakeRect(0, 0, width, STATUSBAR_HEIGHT);
        g_statusBarView = [[StatusBarView alloc] initWithFrame:statusFrame];
        g_statusBarView.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
        [g_statusBarView setWantsLayer:YES];
        [container addSubview:g_statusBarView];

        // HUD now lives as an XR_EXT_window_space_layer composed by the
        // runtime, not as an NSView subview.

        [g_window setContentView:container];
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

    LOG_INFO("Created macOS window (%ux%u) with toolbar + Metal view + status bar", width, height);
    return true;
}

// ============================================================================
// IOSurface creation
// ============================================================================

static bool CreateIOSurface(uint32_t width, uint32_t height, id<MTLDevice> device)
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

    // Create a read-only texture view for blitting to the app's drawable
    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                    width:width
                                                                                   height:height
                                                                                mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;
    g_ioSurfaceReadTexture = [device newTextureWithDescriptor:desc
                                                   iosurface:g_ioSurface
                                                       plane:0];
    if (g_ioSurfaceReadTexture == nil) {
        LOG_ERROR("Failed to create read texture from IOSurface");
        CFRelease(g_ioSurface);
        g_ioSurface = NULL;
        return false;
    }

    LOG_INFO("Created IOSurface: %ux%u, BGRA8, id=%u", width, height, IOSurfaceGetID(g_ioSurface));
    return true;
}

// ============================================================================
// Logical canvas rect (center 50% of the window, in backing pixels)
// ============================================================================

// The 3D canvas is a logical sub-rect of the full-window Metal view —
// center 50%, like the Windows texture apps. Coordinates are TOP-DOWN
// (client-area convention per XR_EXT_win32_window_binding §3.5, matching
// Metal texture space). The pre-#406 code passed AppKit's bottom-up
// `frame.origin.y * scale` as canvas y — it round-tripped only because the
// same value was used for both the runtime write and the app read AND the
// rect is centered; standardize on top-down deliberately.
// Clamped to the shared IOSurface dims (window may exceed the worst-case
// atlas on huge displays).
static void ComputeCanvasRectPx(int32_t *cx, int32_t *cy, uint32_t *cw, uint32_t *ch)
{
    uint32_t winW = 0, winH = 0;
    if (g_window != nil) {
        CGFloat bs = [g_window backingScaleFactor];
        NSSize sz = [[g_window contentView] bounds].size;
        winW = (uint32_t)(sz.width * bs);
        winH = (uint32_t)(sz.height * bs);
    }
    if (winW > g_ioSurfaceWidth) winW = g_ioSurfaceWidth;
    if (winH > g_ioSurfaceHeight) winH = g_ioSurfaceHeight;

    *cx = (int32_t)(winW / 4);
    *cy = (int32_t)(winH / 4);
    *cw = winW / 2;
    *ch = winH / 2;
}

// Window content-area backing pixels, clamped to the shared IOSurface dims
// (the region of the IOSurface the app actually presents).
static void WindowBackingPx(uint32_t *w, uint32_t *h)
{
    *w = 0;
    *h = 0;
    if (g_window != nil) {
        CGFloat bs = [g_window backingScaleFactor];
        NSSize sz = [[g_window contentView] bounds].size;
        *w = (uint32_t)(sz.width * bs);
        *h = (uint32_t)(sz.height * bs);
    }
    if (*w > g_ioSurfaceWidth) *w = g_ioSurfaceWidth;
    if (*h > g_ioSurfaceHeight) *h = g_ioSurfaceHeight;
}

// #439: the rect the app should RENDER for. While the B-mode mask is active
// (post view-size event) the declared canvas is superseded — the effective
// rendering canvas is the full window, top-left anchored: the app
// renegotiates to window-sized rendering (Q4). The declared canvas
// (ComputeCanvasRectPx) is still what the output-rect call and the Tier-2
// mask rect carry.
//
// KNOWN (post-#396-W7 rebase): #396 moved view generation runtime-side (the
// app consumes render-ready XrView{pose,fov}); the compositor's
// get_window_metrics pivots to the window when a mask is active. The texture
// app's old app-side world-scale compensation point was deleted by #396, so
// the B-mode 3D currently renders at a residual scale offset vs the A
// (surround) path inside the canvas region — the surround region stays
// byte-identical and the Q4 event fires correctly. Reconciling a texture
// app's canvas-sized 3D with the runtime-owned window-metric view gen is a
// Windows-D3D11-leg item (where #396 + Phase 3 meet under Leia validation);
// the handle cases (2/3/4) — the headline — are exact and unaffected.
static void EffectiveCanvasRectPx(int32_t *cx, int32_t *cy, uint32_t *cw, uint32_t *ch)
{
    if (g_canvasIsWindow) {
        uint32_t w = 0, h = 0;
        WindowBackingPx(&w, &h);
        *cx = 0;
        *cy = 0;
        *cw = w;
        *ch = h;
        return;
    }
    ComputeCanvasRectPx(cx, cy, cw, ch);
}

// ============================================================================
// 2D surround IOSurface (window-sized, #464)
// ============================================================================

// Fill the surround with a static checkerboard + vertical gradient, with a
// bright border ring just outside the canvas rect so the canvas/surround
// boundary is visually unmistakable (same intent as the Windows app's
// RenderSurroundPattern pixel shader). Pixels inside the canvas rect are
// skipped — the runtime never reads them (the DP owns that region).
// CPU fill is deliberate: no per-frame redraw is required on macOS (no
// keyed-mutex handshake; the IOSurface is coherent), and a static pattern
// keeps captures byte-stable. A Metal-pass + shared-event version is the
// production-shaped follow-up.
// Buffer-based core shared by the IOSurface surround (A path) and the
// B-mode Local2D layer fill — identical bytes so the A/B capture diff is
// pattern-exact. BGRA byte order.
static void FillSurroundPatternBuffer(uint8_t *base, size_t stride, uint32_t w, uint32_t h,
                                      int32_t canvasX, int32_t canvasY,
                                      uint32_t canvasW, uint32_t canvasH)
{
    const uint32_t cell = 64;        // checker cell in pixels
    const uint32_t ring = 6;         // border ring thickness around canvas
    int64_t cL = canvasX, cT = canvasY;
    int64_t cR = canvasX + (int64_t)canvasW, cB = canvasY + (int64_t)canvasH;

    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = base + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
            bool insideCanvas = ((int64_t)x >= cL && (int64_t)x < cR &&
                                 (int64_t)y >= cT && (int64_t)y < cB);
            if (insideCanvas) continue; // DP/mask-owned, never shown

            uint8_t *px = row + (size_t)x * 4;
            bool inRing = ((int64_t)x >= cL - (int64_t)ring && (int64_t)x < cR + (int64_t)ring &&
                           (int64_t)y >= cT - (int64_t)ring && (int64_t)y < cB + (int64_t)ring);
            if (inRing) {
                // Bright amber ring at the canvas boundary
                px[0] = 0;   px[1] = 170; px[2] = 255; px[3] = 255; // BGRA
                continue;
            }
            bool check = (((x / cell) + (y / cell)) & 1) != 0;
            float grad = (h > 1) ? (float)y / (float)(h - 1) : 0.0f;
            uint8_t lo = (uint8_t)(40.0f + 50.0f * grad);
            uint8_t hi = (uint8_t)(90.0f + 90.0f * grad);
            uint8_t v = check ? hi : lo;
            // Teal-tinted checker (BGRA byte order)
            px[0] = v;                       // B
            px[1] = (uint8_t)(v * 3 / 4);    // G
            px[2] = (uint8_t)(v / 2);        // R
            px[3] = 255;                     // A
        }
    }
}

static void FillSurroundPattern(int32_t canvasX, int32_t canvasY,
                                uint32_t canvasW, uint32_t canvasH)
{
    if (g_surroundIOSurface == NULL) return;

    IOSurfaceLock(g_surroundIOSurface, 0, NULL);
    uint8_t *base = (uint8_t *)IOSurfaceGetBaseAddress(g_surroundIOSurface);
    size_t stride = IOSurfaceGetBytesPerRow(g_surroundIOSurface);
    FillSurroundPatternBuffer(base, stride, g_surroundWidth, g_surroundHeight,
                              canvasX, canvasY, canvasW, canvasH);
    IOSurfaceUnlock(g_surroundIOSurface, 0, NULL);
}

// (Re)create the surround IOSurface at the current window backing size.
// Same pixel format as the multiview shared IOSurface ('BGRA') — the
// runtime's strip blit requires strict format equality.
static bool CreateSurroundIOSurface(uint32_t width, uint32_t height)
{
    if (g_surroundIOSurface != NULL) {
        CFRelease(g_surroundIOSurface);
        g_surroundIOSurface = NULL;
    }
    if (width == 0 || height == 0) return false;

    NSDictionary *props = @{
        (id)kIOSurfaceWidth:       @(width),
        (id)kIOSurfaceHeight:      @(height),
        (id)kIOSurfaceBytesPerElement: @(4),
        (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA'),
    };
    g_surroundIOSurface = IOSurfaceCreate((CFDictionaryRef)props);
    if (g_surroundIOSurface == NULL) {
        LOG_ERROR("Failed to create surround IOSurface (%ux%u)", width, height);
        return false;
    }
    g_surroundWidth = width;
    g_surroundHeight = height;
    LOG_INFO("Created surround IOSurface: %ux%u, BGRA8, id=%u",
             width, height, IOSurfaceGetID(g_surroundIOSurface));
    return true;
}

// ============================================================================
// Blit IOSurface to drawable (app's Metal rendering)
// ============================================================================

static void BlitIOSurfaceToDrawable(MetalRenderer &r, CAMetalLayer *metalLayer)
{
    id<CAMetalDrawable> drawable = [metalLayer nextDrawable];
    if (drawable == nil) return;

    MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.colorAttachments[0].texture = drawable.texture;
    rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    rpd.colorAttachments[0].clearColor = MTLClearColorMake(0.08, 0.08, 0.1, 1.0);

    id<MTLCommandBuffer> cmdBuf = [r.commandQueue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:rpd];

    // Position-true 1:1 readback of the shared IOSurface — the drawable IS
    // the full window now (parity with cube_texture_d3d11_win main.cpp):
    //  - surround active:   blit the whole window region (canvas 3D weave +
    //                       runtime-blitted 2D surround strips).
    //  - surround inactive: blit ONLY the canvas sub-rect, at its position;
    //                       the rest of the drawable keeps the clear color.
    if (g_ioSurfaceReadTexture != nil) {
        // Derive the window pixel dims from the drawable itself — always
        // self-consistent, even mid-resize while the compositor adjusts
        // the layer's drawableSize.
        float drawW = (float)drawable.texture.width;
        float drawH = (float)drawable.texture.height;
        float ioW = (float)g_ioSurfaceWidth;
        float ioH = (float)g_ioSurfaceHeight;

        float vpX, vpY, vpW, vpH;
        float uvOffsetX, uvOffsetY, uvScaleX, uvScaleY;
        if (g_surroundRegistered || g_local2DActive) {
            // Full window region, clamped to the worst-case IOSurface.
            // (#439 B-mode: the masked composite fills the window rect —
            // present it whole, exactly like the surround path.)
            vpW = (drawW < ioW) ? drawW : ioW;
            vpH = (drawH < ioH) ? drawH : ioH;
            vpX = 0.0f;
            vpY = 0.0f;
            uvOffsetX = 0.0f;
            uvOffsetY = 0.0f;
            uvScaleX = (ioW > 0) ? vpW / ioW : 1.0f;
            uvScaleY = (ioH > 0) ? vpH / ioH : 1.0f;
        } else {
            // Canvas sub-rect at its position (top-down coords; MTLViewport
            // origin is top-left, so the canvas y drops straight in).
            int32_t cx, cy;
            uint32_t cw, ch;
            ComputeCanvasRectPx(&cx, &cy, &cw, &ch);
            vpX = (float)cx;
            vpY = (float)cy;
            vpW = (float)cw;
            vpH = (float)ch;
            uvOffsetX = (ioW > 0) ? (float)cx / ioW : 0.0f;
            uvOffsetY = (ioH > 0) ? (float)cy / ioH : 0.0f;
            uvScaleX = (ioW > 0) ? (float)cw / ioW : 1.0f;
            uvScaleY = (ioH > 0) ? (float)ch / ioH : 1.0f;
        }

        // Pass UV scale + offset to blit shader
        struct { float uv_scale[2]; float uv_offset[2]; } blitParams = {
            { uvScaleX, uvScaleY },
            { uvOffsetX, uvOffsetY }
        };

        MTLViewport vp = {(double)vpX, (double)vpY, (double)vpW, (double)vpH, 0.0, 1.0};
        [enc setViewport:vp];
        [enc setRenderPipelineState:r.blitPipeline];
        [enc setVertexBytes:&blitParams length:sizeof(blitParams) atIndex:0];
        [enc setFragmentTexture:g_ioSurfaceReadTexture atIndex:0];
        [enc setFragmentSamplerState:r.sampler atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }

    [enc endEncoding];
    [cmdBuf presentDrawable:drawable];
    [cmdBuf commit];
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
                    else if (ch == 'c' && !isRepeat) {
                        g_input.cameraMode = !g_input.cameraMode;
                        if (g_input.cameraMode) {
                            g_input.cameraPosX = 0.0f;
                            g_input.cameraPosY = 0.0f;
                            g_input.cameraPosZ = g_input.nominalViewerZ;
                            g_input.yaw = 0.0f;
                            g_input.pitch = 0.0f;
                            if (g_input.nominalViewerZ > 0.0f)
                                g_input.viewParams.invConvergenceDistance = 1.0f / g_input.nominalViewerZ;
                        } else {
                            g_input.cameraPosX = g_input.cameraPosY = g_input.cameraPosZ = 0.0f;
                            g_input.yaw = 0.0f;
                            g_input.pitch = 0.0f;
                        }
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

        // Update pixel sizes. The Metal view autoresizes to fill the window;
        // the canvas is a LOGICAL center-50% sub-rect (ComputeCanvasRectPx),
        // no view repositioning needed.
        if (g_metalView != nil && g_window != nil) {
            CGFloat backingScale = [g_window backingScaleFactor];
            NSSize winSize = [[g_window contentView] bounds].size;
            g_windowW = (uint32_t)(winSize.width * backingScale);
            g_windowH = (uint32_t)(winSize.height * backingScale);
            int32_t cx, cy;
            uint32_t cw, ch;
            EffectiveCanvasRectPx(&cx, &cy, &cw, &ch);
            g_canvasW = cw;
            g_canvasH = ch;
        }
    }
}

// ============================================================================
// Camera movement
// ============================================================================

static void UpdateCameraMovement(InputState& state, float deltaTime, float displayHeightM = 0.0f) {
    if (state.resetViewRequested) {
        state.yaw = state.pitch = 0.0f;
        float savedVDH = state.viewParams.virtualDisplayHeight;
        bool savedCameraMode = state.cameraMode;
        state.viewParams = ViewParams{};
        state.viewParams.virtualDisplayHeight = savedVDH;
        state.cameraMode = savedCameraMode;
        if (state.cameraMode) {
            state.cameraPosX = 0.0f;
            state.cameraPosY = 0.0f;
            state.cameraPosZ = state.nominalViewerZ;
            if (state.nominalViewerZ > 0.0f)
                state.viewParams.invConvergenceDistance = 1.0f / state.nominalViewerZ;
        } else {
            state.cameraPosX = state.cameraPosY = state.cameraPosZ = 0.0f;
        }
        state.resetViewRequested = false;
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
    std::vector<id<MTLTexture>> images;
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
    bool hasViewRigExt = false;  // XR_EXT_view_rig (#396 W7)
    float displayWidthM;
    float displayHeightM;
    float nominalViewerX, nominalViewerY, nominalViewerZ;
    uint32_t displayPixelWidth, displayPixelHeight;
    uint32_t canvasPixelWidth, canvasPixelHeight;
    float recommendedViewScaleX, recommendedViewScaleY;
    PFN_xrRequestDisplayModeEXT pfnRequestDisplayModeEXT;
    PFN_xrRequestDisplayRenderingModeEXT pfnRequestDisplayRenderingModeEXT;
    PFN_xrEnumerateDisplayRenderingModesEXT pfnEnumerateDisplayRenderingModesEXT;
    PFN_xrSetSharedTextureOutputRectEXT pfnSetSharedTextureOutputRectEXT;
    PFN_xrSetSharedTextureSurround2DEXT pfnSetSharedTextureSurround2DEXT;

    // XR_EXT_local_3d_zone (#439 Phase 3 A/B, B-mode only)
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

    bool hasMetalEnable = false;
    app.hasCocoaWindowBinding = false;
    app.hasDisplayInfoExt = false;

    LOG_INFO("Available OpenXR extensions:");
    for (auto &e : exts) {
        LOG_INFO("  %s v%u", e.extensionName, e.extensionVersion);
        if (strcmp(e.extensionName, XR_KHR_METAL_ENABLE_EXTENSION_NAME) == 0)
            hasMetalEnable = true;
        if (strcmp(e.extensionName, XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME) == 0)
            app.hasCocoaWindowBinding = true;
        if (strcmp(e.extensionName, XR_EXT_LOCAL_3D_ZONE_EXTENSION_NAME) == 0)
            app.hasLocal3DZoneExt = true;
        if (strcmp(e.extensionName, XR_EXT_DISPLAY_INFO_EXTENSION_NAME) == 0)
            app.hasDisplayInfoExt = true;
        if (strcmp(e.extensionName, XR_EXT_VIEW_RIG_EXTENSION_NAME) == 0)
            app.hasViewRigExt = true;
    }

    if (!hasMetalEnable) {
        LOG_ERROR("Runtime does not support XR_KHR_metal_enable");
        return false;
    }
    if (!app.hasCocoaWindowBinding) {
        LOG_ERROR("Runtime does not support XR_EXT_cocoa_window_binding (required for IOSurface mode)");
        return false;
    }
    LOG_INFO("XR_EXT_display_info: %s", app.hasDisplayInfoExt ? "available" : "not available");
    LOG_INFO("XR_EXT_view_rig: %s", app.hasViewRigExt ? "AVAILABLE" : "NOT FOUND");

    // Enable extensions
    std::vector<const char *> enabledExts = {
        XR_KHR_METAL_ENABLE_EXTENSION_NAME,
        XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME
    };
    if (app.hasDisplayInfoExt) {
        enabledExts.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);
    }
    if (app.hasViewRigExt) {
        enabledExts.push_back(XR_EXT_VIEW_RIG_EXTENSION_NAME);
    }
    if (app.hasLocal3DZoneExt) {
        enabledExts.push_back(XR_EXT_LOCAL_3D_ZONE_EXTENSION_NAME);
    }

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(createInfo.applicationInfo.applicationName, "MetalCubeSharedTexture",
            XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.applicationVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = (uint32_t)enabledExts.size();
    createInfo.enabledExtensionNames = enabledExts.data();

    XR_CHECK(xrCreateInstance(&createInfo, &app.instance));
    LOG_INFO("OpenXR instance created");

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
                LOG_INFO("Display info: %.3fx%.3f m, nominal=(%.3f,%.3f,%.3f)",
                    app.displayWidthM, app.displayHeightM,
                    app.nominalViewerX, app.nominalViewerY, app.nominalViewerZ);
            }
        }
        if (app.hasDisplayInfoExt) {
            xrGetInstanceProcAddr(app.instance, "xrRequestDisplayModeEXT",
                (PFN_xrVoidFunction*)&app.pfnRequestDisplayModeEXT);
            xrGetInstanceProcAddr(app.instance, "xrRequestDisplayRenderingModeEXT",
                (PFN_xrVoidFunction*)&app.pfnRequestDisplayRenderingModeEXT);
            xrGetInstanceProcAddr(app.instance, "xrEnumerateDisplayRenderingModesEXT",
                (PFN_xrVoidFunction*)&app.pfnEnumerateDisplayRenderingModesEXT);
            xrGetInstanceProcAddr(app.instance, "xrSetSharedTextureOutputRectEXT",
                (PFN_xrVoidFunction*)&app.pfnSetSharedTextureOutputRectEXT);
            // NULL-tolerant: pre-v6 runtimes don't export the surround call.
            xrGetInstanceProcAddr(app.instance, "xrSetSharedTextureSurround2DEXT",
                (PFN_xrVoidFunction*)&app.pfnSetSharedTextureSurround2DEXT);
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

static bool GetMetalGraphicsRequirements(AppXrSession &app)
{
    PFN_xrGetMetalGraphicsRequirementsKHR xrGetMetalGraphicsRequirementsKHR = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(app.instance, "xrGetMetalGraphicsRequirementsKHR",
                                    (PFN_xrVoidFunction *)&xrGetMetalGraphicsRequirementsKHR));

    XrGraphicsRequirementsMetalKHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
    XR_CHECK(xrGetMetalGraphicsRequirementsKHR(app.instance, app.systemId, &reqs));

    LOG_INFO("Metal graphics requirements: metalDevice=%p", reqs.metalDevice);
    return true;
}

static bool CreateSession(AppXrSession &app, MetalRenderer &r)
{
    LOG_INFO("Creating OpenXR session (texture mode: real view + shared IOSurface)...");

    XrGraphicsBindingMetalKHR metalBinding = {XR_TYPE_GRAPHICS_BINDING_METAL_KHR};
    metalBinding.commandQueue = (__bridge void *)r.commandQueue;

    // Chain the cocoa window binding extension — Texture mode:
    // real NSView (for DP screen-space position tracking / phase alignment)
    // + shared IOSurface (the runtime composites into its canvas sub-rect).
    // Parity with the Windows texture apps (real HWND + shared HANDLE).
    XrCocoaWindowBindingCreateInfoEXT cocoaBinding = {};
    cocoaBinding.type = XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT;
    cocoaBinding.next = nullptr;
    cocoaBinding.viewHandle = (__bridge void *)g_metalView;
    cocoaBinding.sharedIOSurface = (void *)g_ioSurface;

    metalBinding.next = &cocoaBinding;
    LOG_INFO("Chaining XR_EXT_cocoa_window_binding: viewHandle=%p, sharedIOSurface=%p",
             (__bridge void *)g_metalView, (void *)g_ioSurface);

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &metalBinding;
    sessionInfo.systemId = app.systemId;

    XR_CHECK(xrCreateSession(app.instance, &sessionInfo, &app.session));
    LOG_INFO("Session created (texture mode: real view + shared IOSurface)");

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
    // Size swapchain for worst-case atlas across all rendering modes.
    // Use display dims (not canvas) — canvas can grow to full display on window resize.
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

    int64_t selectedFormat = formats[0];
    for (auto f : formats) {
        if (f == (int64_t)MTLPixelFormatBGRA8Unorm) {
            selectedFormat = f;
        }
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

    std::vector<XrSwapchainImageMetalKHR> metalImages(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(app.swapchain.swapchain, imageCount, &imageCount,
                                         (XrSwapchainImageBaseHeader *)metalImages.data()));

    app.swapchain.images.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        app.swapchain.images[i] = (__bridge id<MTLTexture>)metalImages[i].texture;
        LOG_INFO("Swapchain image %u: MTLTexture %p (%lux%lu)",
                 i, metalImages[i].texture,
                 (unsigned long)app.swapchain.images[i].width,
                 (unsigned long)app.swapchain.images[i].height);
    }

    LOG_INFO("Swapchain created: %ux%u, %u images", w, h, imageCount);
    return true;
}

// ============================================================================
// Event handling
// ============================================================================

static void PollEvents(AppXrSession &app)
{
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(app.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
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
        case (XrStructureType)XR_TYPE_EVENT_DATA_LOCAL_3D_ZONE_VIEW_SIZE_CHANGED_EXT: {
            // #439 Phase 3 Q4 — the mask superseded the canvas (or the
            // window resized): the runtime now recommends a new per-view
            // render size. Our projection swapchain is worst-case sized, so
            // no recreate is needed — pivot the rendering canvas to the
            // window and render at the new size next frame.
            auto* vsEvent = (XrEventDataLocal3DZoneViewSizeChangedEXT*)&event;
            LOG_INFO("Local-3D-zone view size changed: %ux%u -> window-sized rendering",
                vsEvent->recommendedImageRectWidth, vsEvent->recommendedImageRectHeight);
            if (g_abLocal2D) {
                g_canvasIsWindow = true;
            }
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

    LOG_INFO("=== Metal Cube OpenXR (IOSurface Shared Texture) ===");

    // #439 Phase 3 A/B + capture-determinism hooks (see g_abLocal2D block).
    {
        const char *e = getenv("DXR_AB_LOCAL2D");
        g_abLocal2D = (e != NULL && e[0] != '\0' && e[0] != '0');
        e = getenv("DXR_FREEZE");
        g_freezeAnimation = (e != NULL && e[0] != '\0' && e[0] != '0');
        e = getenv("DXR_HUD");
        if (e != NULL && e[0] == '0') {
            g_input.hudVisible = false;
        }
        if (g_abLocal2D) {
            LOG_INFO("DXR_AB_LOCAL2D=1 — case-1 B-mode (Tier-2 mask + Local2D layer, no surround)");
        }
    }

    // Initialize Metal renderer
    MetalRenderer renderer = {};
    if (!InitRenderer(renderer)) {
        LOG_ERROR("Failed to initialize Metal renderer");
        return 1;
    }

    // Create the macOS window (app-owned, with toolbar + status bar)
    if (!CreateMacOSWindow(1512, 883, renderer.device)) {  // Extra height for UI chrome
        LOG_ERROR("Failed to create macOS window");
        return 1;
    }

    // Initialize OpenXR (needed before IOSurface creation to know display dimensions)
    AppXrSession app = {};
    if (!InitializeOpenXR(app)) {
        LOG_ERROR("Failed to initialize OpenXR");
        return 1;
    }

    if (!GetMetalGraphicsRequirements(app)) {
        LOG_ERROR("Failed to get Metal graphics requirements");
        return 1;
    }

    // IOSurface = worst-case swapchain atlas (same computation as CreateSwapchain).
    // Canvas rect communicated separately via xrSetSharedTextureOutputRectEXT.
    // See ADR-010 for rationale.
    NSRect backing = [g_metalView convertRectToBacking:g_metalView.bounds];
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
        // Fallback: display dims or window backing
        ioW = app.displayPixelWidth > 0 ? app.displayPixelWidth : (uint32_t)backing.size.width;
        ioH = app.displayPixelHeight > 0 ? app.displayPixelHeight : (uint32_t)backing.size.height;
    }
    LOG_INFO("IOSurface dimensions (worst-case atlas): %ux%u", ioW, ioH);

    // Create the shared IOSurface
    if (!CreateIOSurface(ioW, ioH, renderer.device)) {
        LOG_ERROR("Failed to create IOSurface");
        return 1;
    }

    // Canvas = logical center-50% sub-rect of the window (clamped to the
    // now-known IOSurface dims). The Metal view itself fills the window.
    {
        int32_t cx0, cy0;
        uint32_t cw0, ch0;
        ComputeCanvasRectPx(&cx0, &cy0, &cw0, &ch0);
        app.canvasPixelWidth = cw0;
        app.canvasPixelHeight = ch0;
    }

    // Create session with real view + shared IOSurface (texture mode)
    if (!CreateSession(app, renderer)) {
        LOG_ERROR("Failed to create session");
        return 1;
    }

    // Tell the compositor where the canvas is within the window client area
    if (app.pfnSetSharedTextureOutputRectEXT) {
        int32_t canvasX, canvasY;
        uint32_t canvasW, canvasH;
        ComputeCanvasRectPx(&canvasX, &canvasY, &canvasW, &canvasH);
        app.pfnSetSharedTextureOutputRectEXT(app.session, canvasX, canvasY,
                                              canvasW, canvasH);
        LOG_INFO("Set shared texture output rect: x=%d, y=%d, w=%u, h=%u",
                 canvasX, canvasY, canvasW, canvasH);
    }

    // Register the 2D surround (spec v6) — window-sized per #464. The
    // runtime strip-blits its non-canvas pixels into the shared IOSurface
    // each frame; the app then presents the full window region.
    if (g_abLocal2D) {
        LOG_INFO("A/B B-mode (DXR_AB_LOCAL2D=1): surround registration skipped — "
                 "Tier-2 mask + Local2D layer activate at frame %d", g_local2DActivationFrame);
    } else if (app.pfnSetSharedTextureSurround2DEXT) {
        uint32_t winPxW, winPxH;
        WindowBackingPx(&winPxW, &winPxH);
        if (CreateSurroundIOSurface(winPxW, winPxH)) {
            int32_t cx0, cy0;
            uint32_t cw0, ch0;
            ComputeCanvasRectPx(&cx0, &cy0, &cw0, &ch0);
            FillSurroundPattern(cx0, cy0, cw0, ch0);
            XrResult sres = app.pfnSetSharedTextureSurround2DEXT(
                app.session, (void *)g_surroundIOSurface, winPxW, winPxH);
            g_surroundRegistered = XR_SUCCEEDED(sres);
            LOG_INFO("Surround 2D %s: %ux%u (XrResult=%d)",
                     g_surroundRegistered ? "registered" : "REGISTRATION FAILED",
                     winPxW, winPxH, (int)sres);
        }
    } else {
        LOG_INFO("xrSetSharedTextureSurround2DEXT unavailable — no 2D surround");
    }

    if (!CreateSpaces(app)) {
        LOG_ERROR("Failed to create spaces");
        return 1;
    }

    if (!CreateSwapchain(app)) {
        LOG_ERROR("Failed to create swapchain");
        return 1;
    }

    // #439 A/B B-mode: full-window Local2D layer swapchain (created at
    // activation time so it matches the window backing size then).
    SwapchainInfo l2dSwapchain = {};

    // HUD window-space layer swapchain (XR_EXT_window_space_layer).
    XrHudSwapchain hudSwapchain;
    HudRendererMacOS hudRenderer = {};
    std::vector<id<MTLTexture>> hudSwapchainImages;
    bool hudReady = false;
    if (CreateHudSwapchain(app.session, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT, hudSwapchain)) {
        std::vector<XrSwapchainImageMetalKHR> hudMetal(hudSwapchain.imageCount,
            {XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR});
        if (XR_SUCCEEDED(xrEnumerateSwapchainImages(hudSwapchain.swapchain,
                hudSwapchain.imageCount, &hudSwapchain.imageCount,
                (XrSwapchainImageBaseHeader*)hudMetal.data()))) {
            hudSwapchainImages.resize(hudSwapchain.imageCount);
            for (uint32_t i = 0; i < hudSwapchain.imageCount; i++) {
                hudSwapchainImages[i] = (__bridge id<MTLTexture>)hudMetal[i].texture;
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
    // mode 1 (default of app.currentModeIndex).

    g_input.viewParams.virtualDisplayHeight = 0.24f;
    g_input.nominalViewerZ = app.nominalViewerZ;

    LOG_INFO("Entering main loop... (ESC to quit, drag to rotate, WASD to move, Space to reset)");

    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !app.exitRequested) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        PumpMacOSEvents();

        // Update canvas rect (logical center-50%) for Kooima/weaver alignment,
        // and re-register the window-sized surround on window resize (#464:
        // the surround tracks the window rect, not the worst-case surface).
        if (g_metalView != nil && g_window != nil) {
            int32_t cx, cy;
            uint32_t cw, ch;
            ComputeCanvasRectPx(&cx, &cy, &cw, &ch);

            if (app.pfnSetSharedTextureOutputRectEXT) {
                app.pfnSetSharedTextureOutputRectEXT(app.session, cx, cy, cw, ch);
            }

            if (g_surroundRegistered && app.pfnSetSharedTextureSurround2DEXT) {
                uint32_t winPxW, winPxH;
                WindowBackingPx(&winPxW, &winPxH);
                if (winPxW != g_surroundWidth || winPxH != g_surroundHeight) {
                    if (CreateSurroundIOSurface(winPxW, winPxH)) {
                        FillSurroundPattern(cx, cy, cw, ch);
                        XrResult sres = app.pfnSetSharedTextureSurround2DEXT(
                            app.session, (void *)g_surroundIOSurface, winPxW, winPxH);
                        g_surroundRegistered = XR_SUCCEEDED(sres);
                    } else {
                        app.pfnSetSharedTextureSurround2DEXT(app.session, NULL, 0, 0);
                        g_surroundRegistered = false;
                    }
                }
            }
        }

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

        // Update animation (DXR_FREEZE=1 pins it for byte-stable A/B captures)
        if (!g_freezeAnimation) {
            renderer.cubeRotation += dt * 0.5f;
        }

        // #439 A/B B-mode activation: at frame N create + submit the Tier-2
        // mask (declared canvas rect = the 3D region) and fill the
        // full-window Local2D swapchain with the surround pattern (static
        // content: acquire/fill/release once; the layer references the
        // released image every frame). Deferred to frame N — not startup —
        // so the canvas-supersede changes the runtime's recommended view
        // size and the Q4 view-size event demonstrably fires.
        if (g_abLocal2D && !g_local2DActive && g_frameCounter >= g_local2DActivationFrame &&
            app.hasLocal3DZoneExt && app.pfnCreateLocal3DZoneMaskEXT != nullptr &&
            app.pfnSetLocal3DZoneFromRectsEXT != nullptr && app.pfnSubmitLocal3DZoneEXT != nullptr) {
            uint32_t winW = 0, winH = 0;
            WindowBackingPx(&winW, &winH);
            int32_t mcx = 0, mcy = 0;
            uint32_t mcw = 0, mch = 0;
            ComputeCanvasRectPx(&mcx, &mcy, &mcw, &mch);
            bool ok = (winW > 0 && winH > 0);
            if (ok) {
                XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
                sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
                sci.format = (int64_t)MTLPixelFormatBGRA8Unorm;
                sci.sampleCount = 1;
                sci.width = winW;
                sci.height = winH;
                sci.faceCount = 1;
                sci.arraySize = 1;
                sci.mipCount = 1;
                ok = XR_SUCCEEDED(xrCreateSwapchain(app.session, &sci, &l2dSwapchain.swapchain));
                l2dSwapchain.width = winW;
                l2dSwapchain.height = winH;
            }
            if (ok) {
                uint32_t n = 0;
                xrEnumerateSwapchainImages(l2dSwapchain.swapchain, 0, &n, nullptr);
                std::vector<XrSwapchainImageMetalKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR});
                ok = n > 0 && XR_SUCCEEDED(xrEnumerateSwapchainImages(l2dSwapchain.swapchain, n, &n,
                                                                      (XrSwapchainImageBaseHeader *)imgs.data()));
                if (ok) {
                    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                    uint32_t idx = 0;
                    ok = XR_SUCCEEDED(xrAcquireSwapchainImage(l2dSwapchain.swapchain, &ai, &idx));
                    if (ok) {
                        XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                        wi.timeout = XR_INFINITE_DURATION;
                        xrWaitSwapchainImage(l2dSwapchain.swapchain, &wi);
                        size_t stride = (size_t)winW * 4;
                        uint8_t *buf = (uint8_t *)calloc(1, stride * winH); // canvas hole transparent
                        if (buf != NULL) {
                            FillSurroundPatternBuffer(buf, stride, winW, winH, mcx, mcy, mcw, mch);
                            id<MTLTexture> tex = (__bridge id<MTLTexture>)imgs[idx].texture;
                            [tex replaceRegion:MTLRegionMake2D(0, 0, winW, winH)
                                   mipmapLevel:0
                                     withBytes:buf
                                   bytesPerRow:stride];
                            free(buf);
                        }
                        XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                        xrReleaseSwapchainImage(l2dSwapchain.swapchain, &ri);
                    }
                }
            }
            if (ok) {
                XrLocal3DZoneMaskCreateInfoEXT mci = {
                    (XrStructureType)XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_EXT};
                mci.maskWidth = 0; // runtime picks the window backing size
                mci.maskHeight = 0;
                ok = XR_SUCCEEDED(app.pfnCreateLocal3DZoneMaskEXT(app.session, &mci, &app.local3DZoneMask));
            }
            if (ok) {
                XrRect2Di rect;
                rect.offset = {mcx, mcy};
                rect.extent = {(int32_t)mcw, (int32_t)mch};
                ok = XR_SUCCEEDED(app.pfnSetLocal3DZoneFromRectsEXT(app.local3DZoneMask, 1, &rect)) &&
                     XR_SUCCEEDED(app.pfnSubmitLocal3DZoneEXT(app.local3DZoneMask));
            }
            if (ok) {
                g_local2DActive = true;
                LOG_INFO("A/B B-mode activated: Tier-2 mask (canvas %d,%d %ux%u 3D) + "
                         "full-window Local2D layer %ux%u",
                         mcx, mcy, mcw, mch, winW, winH);
            } else {
                LOG_ERROR("A/B B-mode activation failed — staying on plain projection path");
                g_local2DActivationFrame = 0x7fffffff; // don't retry every frame
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

        // Locate views
        std::vector<XrView> views(app.configViews.size(), {XR_TYPE_VIEW});
        XrViewState viewState = {XR_TYPE_VIEW_STATE};
        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.viewConfigurationType = app.viewConfigType;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = app.localSpace;

        // XR_EXT_view_rig (#396 W7): drive the runtime rig matching the app's
        // current mode (C selects the rig) with the app's tunables — the
        // runtime owns the CANVAS sub-rect resolve and the Kooima math, and
        // returns render-ready XrView{pose, fov}. Per-locate semantics: chain
        // the rig on every consume locate. The raw result struct feeds the
        // HUD's display-space eye readout.
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

        uint32_t modeViewCount = (app.currentModeIndex < app.renderingModeCount)
            ? app.renderingModeViewCounts[app.currentModeIndex] : 2;
        uint32_t tileColumns = (app.currentModeIndex < app.renderingModeCount)
            ? app.renderingModeTileColumns[app.currentModeIndex] : 2;
        uint32_t tileRows = (app.currentModeIndex < app.renderingModeCount)
            ? app.renderingModeTileRows[app.currentModeIndex] : 1;
        bool rendered = false;
        bool display3D = (app.currentModeIndex < app.renderingModeCount)
            ? app.renderingModeDisplay3D[app.currentModeIndex] : true;
        int eyeCount = display3D ? (int)modeViewCount : 1;
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
                renderW = g_canvasW;
                renderH = g_canvasH;
                if (renderW > app.swapchain.width) renderW = app.swapchain.width;
                if (renderH > app.swapchain.height) renderH = app.swapchain.height;
            } else {
                renderW = (uint32_t)(g_canvasW * scaleX);
                renderH = (uint32_t)(g_canvasH * scaleY);
                if (renderW > maxTileW) renderW = maxTileW;
                if (renderH > maxTileH) renderH = maxTileH;
            }
            g_renderW = renderW;
            g_renderH = renderH;

            // --- Consume the runtime's render-ready XrView{pose, fov} ---
            // (#396 W7) The runtime resolves the CANVAS sub-rect itself
            // (get_window_metrics + u_canvas_apply_to_metrics), so the app
            // keeps zero canvas geometry for view generation. Only clip
            // policy stays app-side, by design (fov is clip-independent).
            // Camera rig: same absolute clip as the old app-side camera
            // path. Display rig: ZDP-anchored clip (near = ez - vH, far =
            // ez + 1000·vH; ez = rig-local z of the view pose).
            // mat4_from_xr_fov is Metal [0,1]-native — no remap.
            const float rigVH =
                g_input.viewParams.virtualDisplayHeight / g_input.viewParams.scaleFactor;

            rendered = true;
            std::vector<EyeRenderParams> eyeParams(eyeCount);
            for (int eye = 0; eye < eyeCount; eye++) {
                int viewIdx = eye < (int)viewCount ? eye : 0;
                XrFovf submitFov = views[viewIdx].fov;
                float nearZ = 0.01f, farZ = 100.0f;
                if (useRig && !rigCamera) {
                    float ez = RigLocalEyeZ(rigPose, views[viewIdx].pose.position);
                    nearZ = (ez - rigVH > 0.001f) ? (ez - rigVH) : 0.001f;
                    farZ = ez + 1000.0f * rigVH;
                }
                mat4_view_from_xr_pose(eyeParams[eye].viewMat, views[viewIdx].pose);
                mat4_from_xr_fov(eyeParams[eye].projMat, views[viewIdx].fov, nearZ, farZ);

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
                projViews[eye].pose = views[viewIdx].pose;
                projViews[eye].fov = submitFov;
            }

            RenderScene(renderer, app.swapchain.images[imageIndex], eyeParams.data(), eyeCount);
        }

        // Release swapchain image
        XrSwapchainImageReleaseInfo relInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(app.swapchain.swapchain, &relInfo);

        // Render the HUD into the window-space layer swapchain (when visible).
        // The runtime composes this into the IOSurface alongside the projection
        // layer; the BlitIOSurfaceToDrawable call below carries the result into
        // the app's window.
        bool hudSubmitted = false;
        if (hudReady && g_input.hudVisible && rendered && frameState.shouldRender) {
            uint32_t hudIndex = 0;
            if (AcquireHudSwapchainImage(hudSwapchain, hudIndex)) {
                uint32_t rowPitch = 0;
                const void* pixels = RenderHudAndMap(hudRenderer, &rowPitch,
                    g_hudSessionText, g_hudModeText, g_hudPerfText, g_hudDisplayText,
                    g_hudEyeText, g_hudCameraText, g_hudStereoText, g_hudHelpText);
                if (pixels) {
                    id<MTLTexture> hudTex = hudSwapchainImages[hudIndex];
                    [hudTex replaceRegion:MTLRegionMake2D(0, 0, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT)
                              mipmapLevel:0
                                withBytes:pixels
                              bytesPerRow:rowPitch];
                    UnmapHud(hudRenderer);
                    hudSubmitted = true;
                }
                ReleaseHudSwapchainImage(hudSwapchain);
            }
        }

        // End frame — triggers the compositor to render into the IOSurface
        // (with HUD overlaid when visible) so it's ready for the blit below.
        if (hudSubmitted) {
            float hudAR = (float)HUD_PIXEL_WIDTH / (float)HUD_PIXEL_HEIGHT;
            float canvasAR = (g_canvasW > 0 && g_canvasH > 0)
                ? (float)g_canvasW / (float)g_canvasH : 1.0f;
            float fracW = HUD_WIDTH_FRACTION;
            float fracH = fracW * canvasAR / hudAR;
            if (fracH > 1.0f) { fracH = 1.0f; fracW = hudAR / canvasAR; }
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

            // #439 A/B B-mode: the full-window Local2D layer rides the
            // normal layer list beside the projection layer (the post-weave
            // 2D source — supersedes the surround side-channel).
            XrCompositionLayerLocal2DEXT l2dLayer = {
                (XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT};
            const XrCompositionLayerBaseHeader *layers[2] = {
                (XrCompositionLayerBaseHeader *)&projLayer, nullptr};
            uint32_t layerCount = (rendered && frameState.shouldRender) ? 1 : 0;
            if (layerCount > 0 && g_local2DActive && l2dSwapchain.swapchain != XR_NULL_HANDLE) {
                l2dLayer.layerFlags = 0; // premultiplied; the pattern is opaque
                l2dLayer.subImage.swapchain = l2dSwapchain.swapchain;
                l2dLayer.subImage.imageRect.offset = {0, 0};
                l2dLayer.subImage.imageRect.extent = {(int32_t)l2dSwapchain.width,
                                                      (int32_t)l2dSwapchain.height};
                l2dLayer.subImage.imageArrayIndex = 0;
                l2dLayer.rect.offset = {0, 0};
                l2dLayer.rect.extent = {(int32_t)l2dSwapchain.width, (int32_t)l2dSwapchain.height};
                layers[1] = (XrCompositionLayerBaseHeader *)&l2dLayer;
                layerCount = 2;
            }

            XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = frameState.predictedDisplayTime;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            endInfo.layerCount = layerCount;
            endInfo.layers = layers;

            xrEndFrame(app.session, &endInfo);
        }

        // Now blit the IOSurface content into the app's own drawable
        CAMetalLayer *metalLayer = (CAMetalLayer *)[g_metalView layer];
        if (metalLayer) {
            BlitIOSurfaceToDrawable(renderer, metalLayer);
        }

        g_frameCounter++;

        // FPS tracking
        g_avgFrameTime = g_avgFrameTime * 0.95 + dt * 0.05;

        // Update UI (throttled)
        g_hudUpdateTimer += dt;
        if (g_hudUpdateTimer >= 0.5f) {
            g_hudUpdateTimer = 0.0f;
            @autoreleasepool {
                double fps = (g_avgFrameTime > 0) ? 1.0 / g_avgFrameTime : 0;
                const char *outputModeName = (app.currentModeIndex < app.renderingModeCount)
                    ? app.renderingModeNames[app.currentModeIndex] : "?";
                bool isReq = (app.currentModeIndex < app.renderingModeCount)
                    ? app.renderingModeIsRequestable[app.currentModeIndex] : true;
                const char *lockSuffix = isReq ? "" : " [locked by workspace]";

                // Update toolbar
                if (g_toolbarView != nil) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        g_toolbarView.toolbarText = [NSString stringWithFormat:
                            @"Mode: %s (%s)%s | FPS: %.0f (%.1fms) | IOSurf: %ux%u | Swapchain: %ux%u | Canvas: %ux%u",
                            outputModeName,
                            display3D ? "3D" : "2D", lockSuffix, fps, g_avgFrameTime * 1000.0,
                            g_ioSurfaceWidth, g_ioSurfaceHeight,
                            app.swapchain.width, app.swapchain.height,
                            g_canvasW, g_canvasH];
                        [g_toolbarView setNeedsDisplay:YES];
                    });
                }

                // Update status bar
                if (g_statusBarView != nil) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        const char *modeLabel = g_input.cameraMode ? "Camera" : "Display";
                        NSMutableString *eyeStr = [NSMutableString string];
                        for (uint32_t e = 0; e < app.eyeCount && e < 8; e++) {
                            if (e > 0) [eyeStr appendString:@" "];
                            [eyeStr appendFormat:@"Eye[%u]:(%.3f,%.3f,%.3f)",
                                e, app.eyePositions[e][0], app.eyePositions[e][1], app.eyePositions[e][2]];
                        }
                        g_statusBarView.statusText = [NSString stringWithFormat:
                            @"%@ | %s:(%.2f,%.2f,%.2f) | IPD:%.2f Par:%.2f",
                            eyeStr,
                            modeLabel,
                            g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ,
                            g_input.viewParams.ipdFactor, g_input.viewParams.parallaxFactor];
                        [g_statusBarView setNeedsDisplay:YES];
                    });
                }

                // Refresh cached HUD section strings (consumed each frame by
                // RenderHudAndMap above when HUD is visible). Build via UTF-8
                // → wstring so we don't depend on NSString in the renderer.
                auto utf8ToW = [](const char* s) -> std::wstring {
                    NSString* ns = [NSString stringWithUTF8String:(s ? s : "")];
                    NSData* d = [ns dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
                    size_t n = d.length / sizeof(wchar_t);
                    std::wstring w(n, L'\0');
                    if (n) memcpy((void*)w.data(), d.bytes, d.length);
                    return w;
                };

                const char *sessionStateNames[] = {
                    "UNKNOWN", "IDLE", "READY", "SYNCHRONIZED",
                    "VISIBLE", "FOCUSED", "STOPPING", "LOSS_PENDING", "EXITING"};
                int stateIdx = (int)app.sessionState;
                const char *sessionStateName = (stateIdx >= 0 && stateIdx < 9)
                    ? sessionStateNames[stateIdx] : "INVALID";

                char buf[512];
                snprintf(buf, sizeof(buf), "%s\nSession: %s",
                    app.systemName, sessionStateName);
                g_hudSessionText = utf8ToW(buf);

                const char *kooimaMode = g_input.cameraMode
                    ? "Camera-Centric [C=Toggle]" : "Display-Centric [C=Toggle]";
                snprintf(buf, sizeof(buf),
                    "XR_EXT_cocoa_window_binding: ACTIVE (Real View + IOSurface)\nMode: %s (%s)%s\nKooima: %s",
                    outputModeName, display3D ? "3D" : "2D", lockSuffix, kooimaMode);
                g_hudModeText = utf8ToW(buf);

                snprintf(buf, sizeof(buf),
                    "FPS: %.0f  (%.1f ms)\nRender: %ux%u  Window: %ux%u  Canvas: %ux%u",
                    fps, g_avgFrameTime * 1000.0,
                    g_renderW, g_renderH, g_windowW, g_windowH, g_canvasW, g_canvasH);
                g_hudPerfText = utf8ToW(buf);

                snprintf(buf, sizeof(buf),
                    "Display: %.3f x %.3f m\nNominal: (%.3f, %.3f, %.3f)",
                    app.displayWidthM, app.displayHeightM,
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

                const char *poseLabel = g_input.cameraMode ? "Virtual Camera" : "Virtual Display";
                snprintf(buf, sizeof(buf), "%s: (%.2f, %.2f, %.2f)",
                    poseLabel, g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ);
                g_hudCameraText = utf8ToW(buf);

                const char *param1Label = g_input.cameraMode ? "Conv" : "Persp";
                const char *param2Label = g_input.cameraMode ? "Zoom" : "Scale";
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

                const char *scrollHint = g_input.cameraMode ? "Scroll=Zoom" : "Scroll=Scale";
                const char *perspHint = g_input.cameraMode ? "Opt=Conv" : "Opt=Persp";
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
    }

    LOG_INFO("Shutting down...");

    // Clear the surround registration before tearing the session down
    // (documented lifecycle: clear with NULL, then destroy).
    if (g_surroundRegistered && app.pfnSetSharedTextureSurround2DEXT && app.session) {
        app.pfnSetSharedTextureSurround2DEXT(app.session, NULL, 0, 0);
        g_surroundRegistered = false;
    }

    if (hudReady) {
        CleanupHudRenderer(hudRenderer);
    }
    if (app.local3DZoneMask != XR_NULL_HANDLE && app.pfnDestroyLocal3DZoneMaskEXT != nullptr) {
        app.pfnDestroyLocal3DZoneMaskEXT(app.local3DZoneMask);
    }
    if (l2dSwapchain.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(l2dSwapchain.swapchain);
    }
    if (hudSwapchain.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(hudSwapchain.swapchain);
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

    // Release IOSurfaces
    g_ioSurfaceReadTexture = nil;
    if (g_ioSurface != NULL) {
        CFRelease(g_ioSurface);
        g_ioSurface = NULL;
    }
    if (g_surroundIOSurface != NULL) {
        CFRelease(g_surroundIOSurface);
        g_surroundIOSurface = NULL;
    }

    LOG_INFO("Clean shutdown complete");
    return 0;
}
