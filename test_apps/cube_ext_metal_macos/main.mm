// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Metal OpenXR spinning cube with external window (cocoa_window_binding)
 *
 * Demonstrates XR_EXT_cocoa_window_binding with Metal: the app creates its own
 * NSWindow + CAMetalLayer-backed NSView and passes the NSView to the runtime.
 * The runtime renders into the app's window instead of creating its own.
 *
 * Features:
 * - App creates and owns the NSWindow (XR_EXT_cocoa_window_binding)
 * - Mouse drag camera rotation, scroll zoom, WASD movement
 * - Metal rendering (no Vulkan/MoltenVK dependency)
 * - ESC to quit, Space to reset view
 */

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#define XR_USE_GRAPHICS_API_METAL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_cocoa_window_binding.h>

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>
#include <vector>

#include <unistd.h>
#include <mach-o/dyld.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "display3d_view.h"
#include "camera3d_view.h"
#include "stereo_params.h"
#include <openxr/XR_EXT_display_info.h>

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
    // Metal uses z range [0, 1] (not OpenGL's [-1, 1])
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
// Quaternion helpers (same as cube_ext_vk_macos)
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

// Convert display3d/camera3d projection (OpenGL z[-1,1]) to Metal z[0,1]
static void convert_projection_gl_to_metal(float m[16]) {
    m[2]  = 0.5f * m[2]  + 0.5f * m[3];
    m[6]  = 0.5f * m[6]  + 0.5f * m[7];
    m[10] = 0.5f * m[10] + 0.5f * m[11];
    m[14] = 0.5f * m[14] + 0.5f * m[15];
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
// Metal renderer
// ============================================================================

struct MetalRenderer {
    id<MTLDevice>              device;
    id<MTLCommandQueue>        commandQueue;
    id<MTLLibrary>             shaderLibrary;

    id<MTLRenderPipelineState> cubePipeline;
    id<MTLRenderPipelineState> gridPipeline;
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

// Input state (mirrors cube_ext_vk_macos)
struct InputState {
    float yaw = 0.0f, pitch = 0.0f;
    bool keyW = false, keyA = false, keyS = false, keyD = false;
    bool keyE = false, keyQ = false;
    float cameraPosX = 0.0f, cameraPosY = 0.0f, cameraPosZ = 0.0f;
    bool resetViewRequested = false;
    StereoParams stereo;
    bool hudVisible = true;
    bool displayMode3D = true;
    bool displayModeToggleRequested = false;
    bool renderingModeChangeRequested = false;
    bool cameraMode = false;
    float nominalViewerZ = 0.5f;
};
static InputState g_input;
static const float CAMERA_HALF_TAN_VFOV = 0.32491969623f; // tan(18deg) -> 36deg vFOV

// Current display rendering mode (0=SBS, 1=anaglyph, 2=blend — vendor-defined)
static int g_currentOutputMode = 0;

// Performance stats
static double g_avgFrameTime = 0.0;
static float g_hudUpdateTimer = 0.0f;
static uint32_t g_windowW = 1512, g_windowH = 823;
static uint32_t g_renderW = 0, g_renderH = 0;

static void SignalHandler(int)
{
    g_running = false;
}

// ============================================================================
// HUD overlay (semi-transparent text, rendered as NSView subview)
// ============================================================================

#import "hud_overlay_macos.h"

static HudOverlayView *g_hudView = nil;

// ============================================================================
// macOS window creation (CAMetalLayer-backed NSView)
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

        [g_window setTitle:@"Metal Cube — Metal Native Compositor (External Window)"];
        [g_window setAcceptsMouseMovedEvents:YES];
        [g_window setReleasedWhenClosed:NO];

        g_windowDelegate = [[AppWindowDelegate alloc] init];
        [g_window setDelegate:g_windowDelegate];

        g_metalView = [[AppMetalView alloc] initWithFrame:frame];
        [g_metalView setWantsLayer:YES];

        // Set Retina scale so CAMetalLayer produces pixel-resolution drawables
        CAMetalLayer *metalLayer = (CAMetalLayer *)[g_metalView layer];
        if (metalLayer) {
            metalLayer.contentsScale = [g_window backingScaleFactor];
        }

        [g_window setContentView:g_metalView];
        [g_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        // HUD overlay (semi-transparent text, positioned bottom-left)
        NSRect hudFrame = NSMakeRect(10, 10, 420, 380);
        g_hudView = [[HudOverlayView alloc] initWithFrame:hudFrame];
        [g_metalView addSubview:g_hudView];

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

    LOG_INFO("Created macOS window (%ux%u) with CAMetalLayer-backed view", width, height);
    return true;
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
                    g_input.stereo.ipdFactor *= factor;
                    if (g_input.stereo.ipdFactor < 0.0f) g_input.stereo.ipdFactor = 0.0f;
                    if (g_input.stereo.ipdFactor > 1.0f) g_input.stereo.ipdFactor = 1.0f;
                } else if (scrollMods & NSEventModifierFlagControl) {
                    g_input.stereo.parallaxFactor *= factor;
                    if (g_input.stereo.parallaxFactor < 0.0f) g_input.stereo.parallaxFactor = 0.0f;
                    if (g_input.stereo.parallaxFactor > 1.0f) g_input.stereo.parallaxFactor = 1.0f;
                } else if (scrollMods & NSEventModifierFlagOption) {
                    if (g_input.cameraMode) {
                        g_input.stereo.invConvergenceDistance *= factor;
                        if (g_input.stereo.invConvergenceDistance < 0.1f) g_input.stereo.invConvergenceDistance = 0.1f;
                        if (g_input.stereo.invConvergenceDistance > 10.0f) g_input.stereo.invConvergenceDistance = 10.0f;
                    } else {
                        g_input.stereo.perspectiveFactor *= factor;
                        if (g_input.stereo.perspectiveFactor < 0.1f) g_input.stereo.perspectiveFactor = 0.1f;
                        if (g_input.stereo.perspectiveFactor > 10.0f) g_input.stereo.perspectiveFactor = 10.0f;
                    }
                } else {
                    if (g_input.cameraMode) {
                        g_input.stereo.zoomFactor *= factor;
                        if (g_input.stereo.zoomFactor < 0.1f) g_input.stereo.zoomFactor = 0.1f;
                        if (g_input.stereo.zoomFactor > 10.0f) g_input.stereo.zoomFactor = 10.0f;
                    } else {
                        g_input.stereo.scaleFactor *= factor;
                        if (g_input.stereo.scaleFactor < 0.1f) g_input.stereo.scaleFactor = 0.1f;
                        if (g_input.stereo.scaleFactor > 10.0f) g_input.stereo.scaleFactor = 10.0f;
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
                    else if (ch == '\t' && !isRepeat) { g_input.hudVisible = !g_input.hudVisible; }
                    else if (ch == 'v' && !isRepeat) {
                        g_input.displayMode3D = !g_input.displayMode3D;
                        g_input.displayModeToggleRequested = true;
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
                                g_input.stereo.invConvergenceDistance = 1.0f / g_input.nominalViewerZ;
                        } else {
                            g_input.cameraPosX = g_input.cameraPosY = g_input.cameraPosZ = 0.0f;
                            g_input.yaw = 0.0f;
                            g_input.pitch = 0.0f;
                        }
                    }
                    else if ((ch == '1' || ch == '2' || ch == '3') && !isRepeat) {
                        g_currentOutputMode = ch - '1'; // 0=SBS, 1=anaglyph, 2=blend
                        g_input.renderingModeChangeRequested = true;
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
// Camera movement (ported from cube_ext_vk_macos)
// ============================================================================

static void UpdateCameraMovement(InputState& state, float deltaTime, float displayHeightM = 0.0f) {
    if (state.resetViewRequested) {
        state.yaw = state.pitch = 0.0f;
        float savedVDH = state.stereo.virtualDisplayHeight;
        bool savedCameraMode = state.cameraMode;
        state.stereo = StereoParams{};
        state.stereo.virtualDisplayHeight = savedVDH;
        state.cameraMode = savedCameraMode;
        if (state.cameraMode) {
            state.cameraPosX = 0.0f;
            state.cameraPosY = 0.0f;
            state.cameraPosZ = state.nominalViewerZ;
            if (state.nominalViewerZ > 0.0f)
                state.stereo.invConvergenceDistance = 1.0f / state.nominalViewerZ;
        } else {
            state.cameraPosX = state.cameraPosY = state.cameraPosZ = 0.0f;
        }
        state.resetViewRequested = false;
        return;
    }

    float m2v = 1.0f;
    if (state.stereo.virtualDisplayHeight > 0.0f && displayHeightM > 0.0f)
        m2v = state.stereo.virtualDisplayHeight / displayHeightM;

    const float moveSpeed = 0.1f * m2v / state.stereo.scaleFactor;
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
    float displayWidthM;
    float displayHeightM;
    float nominalViewerX, nominalViewerY, nominalViewerZ;
    uint32_t displayPixelWidth, displayPixelHeight;
    float recommendedViewScaleX, recommendedViewScaleY;
    bool supportsDisplayModeSwitch;
    PFN_xrRequestDisplayModeEXT pfnRequestDisplayModeEXT;
    PFN_xrRequestDisplayRenderingModeEXT pfnRequestDisplayRenderingModeEXT;
    PFN_xrEnumerateDisplayRenderingModesEXT pfnEnumerateDisplayRenderingModesEXT;

    // Enumerated rendering mode names
    uint32_t renderingModeCount;
    char renderingModeNames[8][XR_MAX_SYSTEM_NAME_SIZE];

    // Eye tracking
    float leftEyeX, leftEyeY, leftEyeZ;
    float rightEyeX, rightEyeY, rightEyeZ;
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
        if (strcmp(e.extensionName, XR_EXT_DISPLAY_INFO_EXTENSION_NAME) == 0)
            app.hasDisplayInfoExt = true;
    }

    if (!hasMetalEnable) {
        LOG_ERROR("Runtime does not support XR_KHR_metal_enable");
        return false;
    }
    if (!app.hasCocoaWindowBinding) {
        LOG_WARN("Runtime does not support XR_EXT_cocoa_window_binding — will create own window");
    }
    LOG_INFO("XR_EXT_display_info: %s", app.hasDisplayInfoExt ? "available" : "not available");

    // Enable extensions
    std::vector<const char *> enabledExts = {XR_KHR_METAL_ENABLE_EXTENSION_NAME};
    if (app.hasCocoaWindowBinding) {
        enabledExts.push_back(XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME);
    }
    if (app.hasDisplayInfoExt) {
        enabledExts.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);
    }

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(createInfo.applicationInfo.applicationName, "MetalCubeExtOpenXR",
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
                app.supportsDisplayModeSwitch = displayInfo.supportsDisplayModeSwitch;
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
    LOG_INFO("Creating OpenXR session with Metal binding + cocoa_window_binding...");

    XrGraphicsBindingMetalKHR metalBinding = {XR_TYPE_GRAPHICS_BINDING_METAL_KHR};
    metalBinding.commandQueue = (__bridge void *)r.commandQueue;

    // Chain the cocoa window binding extension — pass our NSView to the runtime
    XrCocoaWindowBindingCreateInfoEXT cocoaBinding = {};
    cocoaBinding.type = XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT;
    cocoaBinding.next = nullptr;
    cocoaBinding.viewHandle = (__bridge void *)g_metalView;

    if (app.hasCocoaWindowBinding) {
        metalBinding.next = &cocoaBinding;
        LOG_INFO("Chaining XR_EXT_cocoa_window_binding with NSView %p", cocoaBinding.viewHandle);
    }

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &metalBinding;
    sessionInfo.systemId = app.systemId;

    XR_CHECK(xrCreateSession(app.instance, &sessionInfo, &app.session));
    LOG_INFO("Session created%s", app.hasCocoaWindowBinding ? " (with external window)" : "");

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
                    LOG_INFO("  [%u] %s", modes[i].modeIndex, modes[i].modeName);
                }
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
    uint32_t w = app.configViews[0].recommendedImageRectWidth * 2;
    uint32_t h = app.configViews[0].recommendedImageRectHeight;

    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(app.session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(app.session, formatCount, &formatCount, formats.data()));

    LOG_INFO("Supported swapchain formats:");
    int64_t selectedFormat = formats[0];
    for (auto f : formats) {
        if (f == (int64_t)MTLPixelFormatBGRA8Unorm) {
            selectedFormat = f;
        }
    }
    for (auto f : formats) {
        LOG_INFO("  format %lld%s", (long long)f, f == selectedFormat ? " (selected)" : "");
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

    LOG_INFO("=== Metal Cube OpenXR (External Window) ===");

    // Initialize Metal renderer
    MetalRenderer renderer = {};
    if (!InitRenderer(renderer)) {
        LOG_ERROR("Failed to initialize Metal renderer");
        return 1;
    }

    // Create the macOS window (app-owned)
    if (!CreateMacOSWindow(1512, 823)) {
        LOG_ERROR("Failed to create macOS window");
        return 1;
    }

    // Initialize OpenXR
    AppXrSession app = {};
    if (!InitializeOpenXR(app)) {
        LOG_ERROR("Failed to initialize OpenXR");
        return 1;
    }

    if (!GetMetalGraphicsRequirements(app)) {
        LOG_ERROR("Failed to get Metal graphics requirements");
        return 1;
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

    // Initialize output mode from env var (vendor-neutral — mode indices)
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

    g_input.stereo.virtualDisplayHeight = 0.24f;
    g_input.nominalViewerZ = app.nominalViewerZ;

    LOG_INFO("Entering main loop... (ESC to quit, drag to rotate, WASD to move, Space to reset)");
    LOG_INFO("Controls: WASD/QE=Move, Drag=Look, Scroll=Scale, Space=Reset, V=2D/3D, C=Mode, Tab=HUD, ESC=Quit");

    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !app.exitRequested) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        PumpMacOSEvents();
        PollEvents(app);

        // Handle display mode toggle (V key)
        if (g_input.displayModeToggleRequested) {
            g_input.displayModeToggleRequested = false;
            if (app.pfnRequestDisplayModeEXT && app.session != XR_NULL_HANDLE) {
                XrDisplayModeEXT mode = g_input.displayMode3D
                    ? XR_DISPLAY_MODE_3D_EXT : XR_DISPLAY_MODE_2D_EXT;
                XrResult modeResult = app.pfnRequestDisplayModeEXT(app.session, mode);
                LOG_INFO("Display mode → %s (%s)",
                    g_input.displayMode3D ? "3D" : "2D",
                    XR_SUCCEEDED(modeResult) ? "OK" : "failed");
            }
        }

        // Handle rendering mode change (1/2/3 keys)
        if (g_input.renderingModeChangeRequested) {
            g_input.renderingModeChangeRequested = false;
            if (app.pfnRequestDisplayRenderingModeEXT && app.session != XR_NULL_HANDLE) {
                const char *modeName = (app.renderingModeCount > 0 && (uint32_t)g_currentOutputMode < app.renderingModeCount)
                    ? app.renderingModeNames[g_currentOutputMode] : "?";
                XrResult res = app.pfnRequestDisplayRenderingModeEXT(app.session, (uint32_t)g_currentOutputMode);
                LOG_INFO("Rendering mode → %s (%s)",
                    modeName,
                    XR_SUCCEEDED(res) ? "OK" : "failed");
            }
        }

        UpdateCameraMovement(g_input, dt, app.displayHeightM);

        if (!app.sessionRunning) {
            usleep(10000);
            continue;
        }

        // Update animation
        renderer.cubeRotation += dt * 0.5f;

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

        std::vector<XrView> views(app.configViews.size(), {XR_TYPE_VIEW});
        XrViewState viewState = {XR_TYPE_VIEW_STATE};
        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.viewConfigurationType = app.viewConfigType;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = app.localSpace;

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

        XrCompositionLayerProjectionView projViews[2] = {};
        bool rendered = false;

        // Render
        if (frameState.shouldRender && viewCount >= 2) {
            // Save raw display-space eye positions for Kooima + HUD
            XrVector3f rawEyePos[2] = {views[0].pose.position, views[1].pose.position};
            app.leftEyeX = rawEyePos[0].x; app.leftEyeY = rawEyePos[0].y; app.leftEyeZ = rawEyePos[0].z;
            app.rightEyeX = rawEyePos[1].x; app.rightEyeY = rawEyePos[1].y; app.rightEyeZ = rawEyePos[1].z;

            int eyeCount = g_input.displayMode3D ? 2 : 1;

            // In mono mode, use center eye
            if (!g_input.displayMode3D) {
                rawEyePos[0] = {
                    (rawEyePos[0].x + rawEyePos[1].x) / 2.0f,
                    (rawEyePos[0].y + rawEyePos[1].y) / 2.0f,
                    (rawEyePos[0].z + rawEyePos[1].z) / 2.0f};
                rawEyePos[1] = rawEyePos[0];
            }

            // Build camera/display pose from player transform
            XrPosef cameraPose;
            quat_from_yaw_pitch(g_input.yaw, g_input.pitch, &cameraPose.orientation);
            cameraPose.position = {g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ};

            XrVector3f nominalViewer = {app.nominalViewerX, app.nominalViewerY, app.nominalViewerZ};

            // Compute render dims
            //   SBS (0):          scaleX=0.5, scaleY=1.0
            //   Anaglyph/Blend:   scaleX=0.5, scaleY=0.5
            float scaleX = 0.5f;
            float scaleY = (g_currentOutputMode == 0) ? 1.0f : 0.5f;
            uint32_t eyeRenderW = app.swapchain.width / 2;
            uint32_t eyeRenderH = app.swapchain.height;
            uint32_t renderW, renderH;
            if (!g_input.displayMode3D) {
                renderW = g_windowW;
                renderH = g_windowH;
                if (renderW > app.swapchain.width) renderW = app.swapchain.width;
                if (renderH > app.swapchain.height) renderH = app.swapchain.height;
            } else {
                renderW = (uint32_t)(g_windowW * scaleX);
                renderH = (uint32_t)(g_windowH * scaleY);
                if (renderW > eyeRenderW) renderW = eyeRenderW;
                if (renderH > eyeRenderH) renderH = eyeRenderH;
            }
            g_renderW = renderW;
            g_renderH = renderH;

            // Compute stereo views using display3d or camera3d library
            Display3DStereoView stereoViews[2];
            bool hasKooima = (app.displayWidthM > 0 && app.displayHeightM > 0);
            if (hasKooima) {
                float dispPxW = app.displayPixelWidth > 0 ? (float)app.displayPixelWidth : (float)app.swapchain.width;
                float dispPxH = app.displayPixelHeight > 0 ? (float)app.displayPixelHeight : (float)app.swapchain.height;
                float pxSizeX = app.displayWidthM / dispPxW;
                float pxSizeY = app.displayHeightM / dispPxH;
                float winW_m = (float)g_windowW * pxSizeX;
                float winH_m = (float)g_windowH * pxSizeY;
                float minDisp = fminf(app.displayWidthM, app.displayHeightM);
                float minWin  = fminf(winW_m, winH_m);
                float vs = minDisp / minWin;
                float screenWidthM  = winW_m * vs;
                float screenHeightM = winH_m * vs;
                // SBS: each eye sees half the display width; anaglyph/blend: full width
                bool sbsMode = g_input.displayMode3D && g_currentOutputMode == 0;
                float baseScreenW = sbsMode ? screenWidthM / 2.0f : screenWidthM;

                Display3DScreen screen;
                screen.width_m = baseScreenW;
                screen.height_m = screenHeightM;

                if (g_input.cameraMode) {
                    Camera3DTunables camTunables;
                    camTunables.ipd_factor = g_input.stereo.ipdFactor;
                    camTunables.parallax_factor = g_input.stereo.parallaxFactor;
                    camTunables.inv_convergence_distance = g_input.stereo.invConvergenceDistance;
                    camTunables.half_tan_vfov = CAMERA_HALF_TAN_VFOV / g_input.stereo.zoomFactor;

                    Camera3DStereoView camViews[2];
                    camera3d_compute_stereo_views(
                        &rawEyePos[0], &rawEyePos[1], &nominalViewer,
                        &screen, &camTunables, &cameraPose,
                        0.01f, 100.0f, &camViews[0], &camViews[1]);

                    for (int i = 0; i < 2; i++) {
                        memcpy(stereoViews[i].view_matrix, camViews[i].view_matrix, sizeof(float) * 16);
                        memcpy(stereoViews[i].projection_matrix, camViews[i].projection_matrix, sizeof(float) * 16);
                        stereoViews[i].fov = camViews[i].fov;
                        stereoViews[i].eye_world = camViews[i].eye_world;
                    }
                } else {
                    Display3DTunables tunables;
                    tunables.ipd_factor = g_input.stereo.ipdFactor;
                    tunables.parallax_factor = g_input.stereo.parallaxFactor;
                    tunables.perspective_factor = g_input.stereo.perspectiveFactor;
                    tunables.virtual_display_height = g_input.stereo.virtualDisplayHeight / g_input.stereo.scaleFactor;

                    display3d_compute_stereo_views(
                        &rawEyePos[0], &rawEyePos[1], &nominalViewer,
                        &screen, &tunables, &cameraPose,
                        0.01f, 100.0f, &stereoViews[0], &stereoViews[1]);
                }

                // Convert projection matrices from OpenGL z[-1,1] to Metal z[0,1]
                convert_projection_gl_to_metal(stereoViews[0].projection_matrix);
                convert_projection_gl_to_metal(stereoViews[1].projection_matrix);
            }

            rendered = true;
            EyeRenderParams eyeParams[2];
            for (int eye = 0; eye < eyeCount; eye++) {
                XrFovf submitFov = views[eye].fov;
                if (hasKooima) {
                    memcpy(eyeParams[eye].viewMat, stereoViews[eye].view_matrix, sizeof(float) * 16);
                    memcpy(eyeParams[eye].projMat, stereoViews[eye].projection_matrix, sizeof(float) * 16);
                    submitFov = stereoViews[eye].fov;
                    views[eye].pose.position = stereoViews[eye].eye_world;
                    views[eye].pose.orientation = cameraPose.orientation;
                } else {
                    mat4_view_from_xr_pose(eyeParams[eye].viewMat, views[eye].pose);
                    mat4_from_xr_fov(eyeParams[eye].projMat, views[eye].fov, 0.01f, 100.0f);
                }

                uint32_t vpX = g_input.displayMode3D ? (eye * renderW) : 0;
                eyeParams[eye].viewportX = vpX;
                eyeParams[eye].viewportY = 0;
                eyeParams[eye].width = renderW;
                eyeParams[eye].height = renderH;

                projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                projViews[eye].subImage.swapchain = app.swapchain.swapchain;
                projViews[eye].subImage.imageRect.offset = {(int32_t)vpX, 0};
                projViews[eye].subImage.imageRect.extent = {
                    (int32_t)renderW, (int32_t)renderH};
                projViews[eye].subImage.imageArrayIndex = 0;
                projViews[eye].pose = views[eye].pose;
                projViews[eye].fov = submitFov;
            }

            RenderScene(renderer, app.swapchain.images[imageIndex], eyeParams, eyeCount);
        }

        // Release swapchain image
        XrSwapchainImageReleaseInfo relInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(app.swapchain.swapchain, &relInfo);

        // End frame
        {
            uint32_t submitCount = g_input.displayMode3D ? 2u : 1u;
            XrCompositionLayerProjection projLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projLayer.space = app.localSpace;
            projLayer.viewCount = submitCount;
            projLayer.views = projViews;

            const XrCompositionLayerBaseHeader *layers[] = {
                (XrCompositionLayerBaseHeader *)&projLayer
            };

            XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = frameState.predictedDisplayTime;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            endInfo.layerCount = (rendered && frameState.shouldRender) ? 1 : 0;
            endInfo.layers = layers;

            xrEndFrame(app.session, &endInfo);
        }

        // FPS tracking
        g_avgFrameTime = g_avgFrameTime * 0.95 + dt * 0.05;

        // Update HUD overlay (throttled)
        g_hudUpdateTimer += dt;
        if (g_hudUpdateTimer >= 0.5f) {
            g_hudUpdateTimer = 0.0f;
            @autoreleasepool {
                if (g_input.hudVisible && g_hudView != nil) {
                    double fps = (g_avgFrameTime > 0) ? 1.0 / g_avgFrameTime : 0;
                    const char *sessionStateNames[] = {
                        "UNKNOWN", "IDLE", "READY", "SYNCHRONIZED",
                        "VISIBLE", "FOCUSED", "STOPPING", "LOSS_PENDING", "EXITING"};
                    int stateIdx = (int)app.sessionState;
                    const char *sessionStateName = (stateIdx >= 0 && stateIdx < 9)
                        ? sessionStateNames[stateIdx] : "INVALID";

                    const char *poseLabel = g_input.cameraMode ? "Virtual Camera" : "Virtual Display";
                    const char *param1Label = g_input.cameraMode ? "Conv" : "Persp";
                    const char *param2Label = g_input.cameraMode ? "Zoom" : "Scale";
                    const char *kooimaMode = g_input.cameraMode ? "Camera-Centric [C=Toggle]" : "Display-Centric [C=Toggle]";
                    const char *scrollHint = g_input.cameraMode ? "Scroll=Zoom" : "Scroll=Scale";
                    const char *perspHint = g_input.cameraMode ? "Opt=Conv" : "Opt=Persp";

                    char valueLine[64];
                    if (g_input.cameraMode) {
                        float tanHFOV = CAMERA_HALF_TAN_VFOV / g_input.stereo.zoomFactor;
                        snprintf(valueLine, sizeof(valueLine), "tanHFOV: %.3f", tanHFOV);
                    } else {
                        float m2v = (g_input.stereo.virtualDisplayHeight > 0.0f && app.displayHeightM > 0.0f)
                            ? g_input.stereo.virtualDisplayHeight / app.displayHeightM : 1.0f;
                        snprintf(valueLine, sizeof(valueLine), "vHeight: %.3f  m2v: %.3f",
                            g_input.stereo.virtualDisplayHeight, m2v);
                    }

                    const char *outputModeName = (app.renderingModeCount > 0 && (uint32_t)g_currentOutputMode < app.renderingModeCount)
                        ? app.renderingModeNames[g_currentOutputMode] : "?";

                    // Build output mode hint: "1-N=Output" if >1 mode, empty if single
                    char outputHint[32] = "";
                    if (app.renderingModeCount > 1) {
                        snprintf(outputHint, sizeof(outputHint), "  1-%u=Output", app.renderingModeCount);
                    }

                    NSString *text = [NSString stringWithFormat:
                        @"%s\n"
                        "Session: %s\n"
                        "Mode: %s  Output: %s\n"
                        "Kooima: %s\n"
                        "FPS: %.0f  (%.1f ms)\n"
                        "Render: %ux%u  Window: %ux%u\n"
                        "Display: %.3f x %.3f m\n"
                        "Scale: %.2f x %.2f\n"
                        "Nominal: (%.3f, %.3f, %.3f)\n"
                        "Eye L: (%.3f, %.3f, %.3f)\n"
                        "Eye R: (%.3f, %.3f, %.3f)\n"
                        "%s: (%.2f, %.2f, %.2f)\n"
                        "IPD: %.2f  Parallax: %.2f\n"
                        "%s: %.2f  %s: %.2f\n"
                        "%s\n"
                        "\n"
                        "WASD/QE=Move  Drag=Look  Space=Reset\n"
                        "%s  Shift=IPD  Ctrl=Parallax  %s\n"
                        "C=Mode  V=2D/3D%s  Tab=HUD  ESC=Quit",
                        app.systemName,
                        sessionStateName,
                        g_input.displayMode3D ? "3D (Stereo)" : "2D (Mono)",
                        outputModeName,
                        kooimaMode,
                        fps, g_avgFrameTime * 1000.0,
                        g_renderW, g_renderH,
                        g_windowW, g_windowH,
                        app.displayWidthM, app.displayHeightM,
                        app.recommendedViewScaleX, app.recommendedViewScaleY,
                        app.nominalViewerX, app.nominalViewerY, app.nominalViewerZ,
                        app.leftEyeX, app.leftEyeY, app.leftEyeZ,
                        app.rightEyeX, app.rightEyeY, app.rightEyeZ,
                        poseLabel,
                        g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ,
                        g_input.stereo.ipdFactor, g_input.stereo.parallaxFactor,
                        param1Label, g_input.cameraMode ? g_input.stereo.invConvergenceDistance : g_input.stereo.perspectiveFactor,
                        param2Label, g_input.cameraMode ? g_input.stereo.zoomFactor : g_input.stereo.scaleFactor,
                        valueLine,
                        scrollHint, perspHint,
                        outputHint];
                    g_hudView.hudText = text;
                    [g_hudView setNeedsDisplay:YES];
                    [g_hudView setHidden:NO];
                } else if (g_hudView != nil) {
                    [g_hudView setHidden:YES];
                }
            }
        }
    }

    LOG_INFO("Shutting down...");

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

    LOG_INFO("Clean shutdown complete");
    return 0;
}
