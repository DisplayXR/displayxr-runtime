// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Metal OpenXR spinning cube test app for macOS
 *
 * Self-contained single-file app that renders a spinning cube + grid floor
 * via Metal + OpenXR. Mirrors cube_vk_macos but uses Metal natively.
 *
 * No windowing code — the runtime's compositor creates its own window.
 * No input handling — static camera with continuous cube rotation.
 */

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#define XR_USE_GRAPHICS_API_METAL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_display_info.h>

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
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

static void mat4_view_from_xr_pose(float m[16], const XrPosef &pose)
{
    // Quaternion to rotation matrix (transposed = inverse rotation)
    float x = pose.orientation.x, y = pose.orientation.y;
    float z = pose.orientation.z, w = pose.orientation.w;

    float r00 = 1 - 2*(y*y + z*z), r01 = 2*(x*y + w*z),     r02 = 2*(x*z - w*y);
    float r10 = 2*(x*y - w*z),     r11 = 1 - 2*(x*x + z*z), r12 = 2*(y*z + w*x);
    float r20 = 2*(x*z + w*y),     r21 = 2*(y*z - w*x),      r22 = 1 - 2*(x*x + y*y);

    float px = pose.position.x, py = pose.position.y, pz = pose.position.z;

    mat4_identity(m);
    // Transposed rotation (inverse)
    m[0] = r00; m[1] = r10; m[2]  = r20;
    m[4] = r01; m[5] = r11; m[6]  = r21;
    m[8] = r02; m[9] = r12; m[10] = r22;
    // -R^T * p
    m[12] = -(r00*px + r01*py + r02*pz);
    m[13] = -(r10*px + r11*py + r12*pz);
    m[14] = -(r20*px + r21*py + r22*pz);
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
        // Z-parallel line
        verts.push_back({{f, Y, -N * S}});
        verts.push_back({{f, Y,  N * S}});
        // X-parallel line
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

    // Pipelines
    id<MTLRenderPipelineState> cubePipeline;
    id<MTLRenderPipelineState> gridPipeline;
    id<MTLDepthStencilState>   depthState;

    // Geometry
    id<MTLBuffer>              cubeVertexBuffer;
    id<MTLBuffer>              cubeIndexBuffer;
    id<MTLBuffer>              gridVertexBuffer;
    int                        gridVertexCount;

    // Textures
    id<MTLTexture>             textures[3]; // basecolor, normal, AO
    id<MTLSamplerState>        sampler;
    bool                       texturesLoaded;

    // Depth
    id<MTLTexture>             depthTexture;
    uint32_t                   depthWidth, depthHeight;

    // Animation
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

    // Compile shaders
    NSError *error = nil;
    r.shaderLibrary = [r.device newLibraryWithSource:[NSString stringWithUTF8String:g_metalShaderSource]
                                             options:nil
                                               error:&error];
    if (!r.shaderLibrary) {
        LOG_ERROR("Shader compilation failed: %s", error.localizedDescription.UTF8String);
        return false;
    }

    // Vertex descriptors
    // Cube vertex layout
    MTLVertexDescriptor *cubeVertDesc = [[MTLVertexDescriptor alloc] init];
    cubeVertDesc.attributes[0].format = MTLVertexFormatFloat3;  // pos
    cubeVertDesc.attributes[0].offset = offsetof(CubeVertex, pos);
    cubeVertDesc.attributes[0].bufferIndex = 0;
    cubeVertDesc.attributes[1].format = MTLVertexFormatFloat4;  // color
    cubeVertDesc.attributes[1].offset = offsetof(CubeVertex, color);
    cubeVertDesc.attributes[1].bufferIndex = 0;
    cubeVertDesc.attributes[2].format = MTLVertexFormatFloat2;  // uv
    cubeVertDesc.attributes[2].offset = offsetof(CubeVertex, uv);
    cubeVertDesc.attributes[2].bufferIndex = 0;
    cubeVertDesc.attributes[3].format = MTLVertexFormatFloat3;  // normal
    cubeVertDesc.attributes[3].offset = offsetof(CubeVertex, normal);
    cubeVertDesc.attributes[3].bufferIndex = 0;
    cubeVertDesc.attributes[4].format = MTLVertexFormatFloat3;  // tangent
    cubeVertDesc.attributes[4].offset = offsetof(CubeVertex, tangent);
    cubeVertDesc.attributes[4].bufferIndex = 0;
    cubeVertDesc.layouts[0].stride = sizeof(CubeVertex);

    // Grid vertex layout
    MTLVertexDescriptor *gridVertDesc = [[MTLVertexDescriptor alloc] init];
    gridVertDesc.attributes[0].format = MTLVertexFormatFloat3;  // pos
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
    rpd.colorAttachments[0].clearColor = MTLClearColorMake(0.05, 0.05, 0.1, 1.0);
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

        // View-projection matrix
        float vp_mat[16];
        mat4_multiply(vp_mat, eye.projMat, eye.viewMat);

        // --- Draw cube ---
        {
            float model[16], rotation[16], translation[16], scale[16], tmp[16];
            mat4_scaling(scale, 0.3f);
            mat4_rotation_y(rotation, r.cubeRotation);
            mat4_translation(translation, 0.0f, 1.6f, -2.0f);
            mat4_multiply(tmp, rotation, scale);
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
            GridUniforms uniforms;
            memcpy(uniforms.mvp, vp_mat, sizeof(vp_mat));
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

    // Rendering mode enumeration (XR_EXT_display_info)
    PFN_xrEnumerateDisplayRenderingModesEXT pfnEnumerateDisplayRenderingModesEXT = nullptr;
    uint32_t displayPixelWidth = 0, displayPixelHeight = 0;
    uint32_t renderingModeCount = 0;
    uint32_t renderingModeTileColumns[8] = {};
    uint32_t renderingModeTileRows[8] = {};
    float renderingModeScaleX[8] = {};
    float renderingModeScaleY[8] = {};
    uint32_t renderingModeViewCounts[8] = {};
    uint32_t currentModeIndex = 1;  // Default: mode 1 (first 3D mode, matches runtime default)
};

static volatile bool g_running = true;

static void SignalHandler(int)
{
    g_running = false;
}

// ============================================================================
// OpenXR initialization
// ============================================================================

static bool InitializeOpenXR(AppXrSession &app)
{
    // Enumerate extensions
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES, nullptr, "", 0});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    bool hasMetalEnable = false;
    bool hasDisplayInfoExt = false;
    LOG_INFO("Available OpenXR extensions:");
    for (auto &e : exts) {
        LOG_INFO("  %s v%u", e.extensionName, e.extensionVersion);
        if (strcmp(e.extensionName, XR_KHR_METAL_ENABLE_EXTENSION_NAME) == 0)
            hasMetalEnable = true;
        if (strcmp(e.extensionName, "XR_EXT_display_info") == 0)
            hasDisplayInfoExt = true;
    }

    if (!hasMetalEnable) {
        LOG_ERROR("Runtime does not support XR_KHR_metal_enable");
        return false;
    }

    // Create instance
    std::vector<const char*> enabledExts = {XR_KHR_METAL_ENABLE_EXTENSION_NAME};
    if (hasDisplayInfoExt)
        enabledExts.push_back("XR_EXT_display_info");
    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(createInfo.applicationInfo.applicationName, "MetalCubeOpenXR",
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

    // Query display pixel dimensions for swapchain sizing
    {
        XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayInfoEXT displayInfo = {XR_TYPE_DISPLAY_INFO_EXT};
        sysProps.next = &displayInfo;
        if (XR_SUCCEEDED(xrGetSystemProperties(app.instance, app.systemId, &sysProps))) {
            app.displayPixelWidth = displayInfo.displayPixelWidth;
            app.displayPixelHeight = displayInfo.displayPixelHeight;
            LOG_INFO("Display pixels: %ux%u", app.displayPixelWidth, app.displayPixelHeight);
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
    XrGraphicsBindingMetalKHR binding = {XR_TYPE_GRAPHICS_BINDING_METAL_KHR};
    binding.commandQueue = (__bridge void *)r.commandQueue;

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = app.systemId;

    XR_CHECK(xrCreateSession(app.instance, &sessionInfo, &app.session));
    LOG_INFO("OpenXR session created with Metal binding");

    app.sessionState = XR_SESSION_STATE_UNKNOWN;
    app.sessionRunning = false;
    app.exitRequested = false;

    // Get rendering mode enumeration function pointer
    xrGetInstanceProcAddr(app.instance, "xrEnumerateDisplayRenderingModesEXT",
        (PFN_xrVoidFunction*)&app.pfnEnumerateDisplayRenderingModesEXT);

    // Enumerate rendering modes for tile layout info
    if (app.pfnEnumerateDisplayRenderingModesEXT) {
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
                    app.renderingModeViewCounts[i] = modes[i].viewCount;
                    app.renderingModeTileColumns[i] = modes[i].tileColumns;
                    app.renderingModeTileRows[i] = modes[i].tileRows;
                    app.renderingModeScaleX[i] = modes[i].viewScaleX;
                    app.renderingModeScaleY[i] = modes[i].viewScaleY;
                    LOG_INFO("  Mode %u: '%s' views=%u tiles=%ux%u scale=%.2fx%.2f",
                        i, modes[i].modeName, modes[i].viewCount,
                        modes[i].tileColumns, modes[i].tileRows,
                        modes[i].viewScaleX, modes[i].viewScaleY);
                }
            }
        }
    }

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

    // Query supported formats
    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(app.session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(app.session, formatCount, &formatCount, formats.data()));

    LOG_INFO("Supported swapchain formats:");
    // Prefer BGRA8Unorm (80) to match our pipeline pixel format
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

    // Enumerate swapchain images
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
        case XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_EXT: {
            auto *rmc = (XrEventDataRenderingModeChangedEXT *)&event;
            app.currentModeIndex = rmc->currentModeIndex;
            LOG_INFO("Rendering mode changed: %u -> %u", rmc->previousModeIndex, rmc->currentModeIndex);
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

    LOG_INFO("=== Metal Cube OpenXR Test App ===");

    // Initialize Metal renderer
    MetalRenderer renderer = {};
    if (!InitRenderer(renderer)) {
        LOG_ERROR("Failed to initialize Metal renderer");
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

    LOG_INFO("Entering main loop...");

    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !app.exitRequested) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        PollEvents(app);

        if (!app.sessionRunning) {
            usleep(10000); // 10ms sleep when not rendering
            continue;
        }

        // Update animation
        renderer.cubeRotation += dt * 0.5f;

        // Wait frame
        XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState = {XR_TYPE_FRAME_STATE};
        XrResult waitRes = xrWaitFrame(app.session, &waitInfo, &frameState);
        if (XR_FAILED(waitRes)) {
            LOG_WARN("xrWaitFrame failed");
            continue;
        }

        XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
        XrResult beginRes = xrBeginFrame(app.session, &beginInfo);
        if (XR_FAILED(beginRes)) {
            LOG_WARN("xrBeginFrame failed");
            continue;
        }

        // Locate views
        std::vector<XrView> views(app.configViews.size(), {XR_TYPE_VIEW});
        XrViewState viewState = {XR_TYPE_VIEW_STATE};
        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.viewConfigurationType = app.viewConfigType;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = app.localSpace;

        uint32_t viewCount = 0;
        xrLocateViews(app.session, &locateInfo, &viewState, (uint32_t)views.size(), &viewCount, views.data());

        // Log view poses periodically (every 120 frames ~2s)
        static int logCounter = 0;
        if (logCounter % 120 == 0 && viewCount >= 2) {
            for (uint32_t i = 0; i < viewCount && i < 2; i++) {
                auto &p = views[i].pose.position;
                auto &o = views[i].pose.orientation;
                auto &f = views[i].fov;
                LOG_INFO("View[%u] pos=(%.3f,%.3f,%.3f) ori=(%.3f,%.3f,%.3f,%.3f) fov=(L%.1f R%.1f U%.1f D%.1f)",
                    i, p.x, p.y, p.z, o.x, o.y, o.z, o.w,
                    f.angleLeft*57.3f, f.angleRight*57.3f, f.angleUp*57.3f, f.angleDown*57.3f);
            }
            LOG_INFO("shouldRender=%d viewState.flags=0x%llx viewCount=%u",
                frameState.shouldRender, (unsigned long long)viewState.viewStateFlags, viewCount);
        }
        logCounter++;

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

        // Use current mode's view count (not xrLocateViews count, which is max across all modes)
        uint32_t modeViewCount = (app.currentModeIndex < app.renderingModeCount)
            ? app.renderingModeViewCounts[app.currentModeIndex] : viewCount;
        // Render N views into tile positions using runtime-provided tile layout.
        // Falls back to derived layout if mode enumeration unavailable.
        uint32_t tileColumns = (app.currentModeIndex < app.renderingModeCount)
            ? app.renderingModeTileColumns[app.currentModeIndex] : (modeViewCount >= 2 ? 2 : 1);
        uint32_t tileRows = (app.currentModeIndex < app.renderingModeCount)
            ? app.renderingModeTileRows[app.currentModeIndex] : ((modeViewCount + tileColumns - 1) / tileColumns);
        if (frameState.shouldRender && modeViewCount >= 1) {
            uint32_t eyeW = tileColumns > 0 ? app.swapchain.width / tileColumns : app.swapchain.width;
            uint32_t eyeH = tileRows > 0 ? app.swapchain.height / tileRows : app.swapchain.height;

            std::vector<EyeRenderParams> eyeParams(modeViewCount);
            for (uint32_t i = 0; i < modeViewCount; i++) {
                uint32_t tileX = i % tileColumns;
                uint32_t tileY = i / tileColumns;
                eyeParams[i].viewportX = tileX * eyeW;
                eyeParams[i].viewportY = tileY * eyeH;
                eyeParams[i].width = eyeW;
                eyeParams[i].height = eyeH;
                mat4_view_from_xr_pose(eyeParams[i].viewMat, views[i].pose);
                mat4_from_xr_fov(eyeParams[i].projMat, views[i].fov, 0.05f, 100.0f);
            }

            RenderScene(renderer, app.swapchain.images[imageIndex], eyeParams.data(), (int)modeViewCount);
        }

        // Release swapchain image
        XrSwapchainImageReleaseInfo relInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(app.swapchain.swapchain, &relInfo);

        // End frame
        std::vector<XrCompositionLayerProjectionView> projViews(modeViewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
        for (uint32_t i = 0; i < modeViewCount; i++) {
            uint32_t tileX = i % tileColumns;
            uint32_t tileY = i / tileColumns;
            uint32_t eyeW = tileColumns > 0 ? app.swapchain.width / tileColumns : app.swapchain.width;
            uint32_t eyeH = tileRows > 0 ? app.swapchain.height / tileRows : app.swapchain.height;
            projViews[i].pose = views[i].pose;
            projViews[i].fov = views[i].fov;
            projViews[i].subImage.swapchain = app.swapchain.swapchain;
            projViews[i].subImage.imageRect.offset = {(int32_t)(tileX * eyeW), (int32_t)(tileY * eyeH)};
            projViews[i].subImage.imageRect.extent = {
                (int32_t)eyeW,
                (int32_t)eyeH
            };
            projViews[i].subImage.imageArrayIndex = 0;
        }

        XrCompositionLayerProjection projLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projLayer.space = app.localSpace;
        projLayer.viewCount = modeViewCount;
        projLayer.views = projViews.data();

        const XrCompositionLayerBaseHeader *layers[] = {
            (XrCompositionLayerBaseHeader *)&projLayer
        };

        XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = frameState.shouldRender ? 1 : 0;
        endInfo.layers = layers;

        xrEndFrame(app.session, &endInfo);
    }

    LOG_INFO("Shutting down...");

    // Cleanup
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
