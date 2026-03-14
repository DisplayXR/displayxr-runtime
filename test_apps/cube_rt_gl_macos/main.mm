// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenGL OpenXR spinning cube test app for macOS (non-ext)
 *
 * Self-contained single-file app that renders a spinning cube + grid floor
 * via OpenGL + OpenXR. Mirrors cube_metal_macos but uses OpenGL natively.
 *
 * No windowing code — the runtime's compositor creates its own window.
 * No input handling — static camera with continuous cube rotation.
 *
 * Uses XR_EXT_macos_gl_binding to provide a CGL context to the runtime.
 * Swapchain textures are IOSurface-backed GL_TEXTURE_RECTANGLE.
 */

#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>

#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_macos_gl_binding.h>
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

// Cube vertex shader — uses GL_TEXTURE_RECTANGLE (unnormalized coords)
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

// Grid shaders
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
// GL shader compilation helpers
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

    // Cube uniforms
    GLint cubeLocMVP, cubeLocModel, cubeLocTexSize;
    GLint cubeLocBasecolor, cubeLocNormal, cubeLocAO;

    // Grid uniforms
    GLint gridLocMVP, gridLocColor;

    // Geometry
    GLuint cubeVAO, cubeVBO, cubeEBO;
    GLuint gridVAO, gridVBO;
    int gridVertexCount;

    // Textures (GL_TEXTURE_RECTANGLE for loaded images)
    GLuint textures[3]; // basecolor, normal, AO
    int texSizes[3][2]; // width, height per texture

    // Depth buffer (renderbuffer)
    GLuint depthRBO;
    GLuint fbo;
    uint32_t depthWidth, depthHeight;

    float cubeRotation;
};

// ============================================================================
// Texture loading (into GL_TEXTURE_RECTANGLE)
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

    // Load textures
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

// ============================================================================
// Render scene into swapchain texture (GL_TEXTURE_RECTANGLE)
// ============================================================================

static void RenderScene(GLRenderer &r, GLuint targetTex, uint32_t targetW, uint32_t targetH,
                         const EyeRenderParams *eyes, int eyeCount)
{
    EnsureFBO(r, targetW, targetH);

    // Bind FBO with swapchain texture as color attachment
    glBindFramebuffer(GL_FRAMEBUFFER, r.fbo);
    // Swapchain textures are GL_TEXTURE_2D when using GL native compositor,
    // GL_TEXTURE_RECTANGLE when using Metal compositor (IOSurface-backed).
    // Detect by checking if the texture is bound to TEXTURE_2D or TEXTURE_RECTANGLE.
    GLint prev_tex2d = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex2d);
    glBindTexture(GL_TEXTURE_2D, targetTex);
    GLint tex_width = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tex_width);
    glBindTexture(GL_TEXTURE_2D, prev_tex2d);
    GLenum texTarget = (tex_width > 0) ? GL_TEXTURE_2D : GL_TEXTURE_RECTANGLE;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texTarget, targetTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, r.depthRBO);

    GLenum fbStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("Framebuffer incomplete: 0x%x", fbStatus);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
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
            float model[16], rotation[16], translation[16], scale[16], tmp[16];
            mat4_scaling(scale, 0.3f);
            mat4_rotation_y(rotation, r.cubeRotation);
            mat4_translation(translation, 0.0f, 1.6f, -2.0f);
            mat4_multiply(tmp, rotation, scale);
            mat4_multiply(model, translation, tmp);

            float mvp[16];
            mat4_multiply(mvp, vp_mat, model);

            glUseProgram(r.cubeProgram);
            glUniformMatrix4fv(r.cubeLocMVP, 1, GL_FALSE, mvp);
            glUniformMatrix4fv(r.cubeLocModel, 1, GL_FALSE, model);
            // Pass basecolor texture size for TEXTURE_RECTANGLE UV scaling
            glUniform2f(r.cubeLocTexSize,
                        (float)r.texSizes[0][0], (float)r.texSizes[0][1]);

            // Bind textures
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
            float gridMvp[16];
            mat4_multiply(gridMvp, vp_mat, (float[16]){1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1});

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
// OpenXR session management
// ============================================================================

struct SwapchainInfo {
    XrSwapchain swapchain;
    int64_t format;
    uint32_t width, height, imageCount;
    std::vector<GLuint> images; // GL texture names
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
    uint32_t currentModeIndex = 0;
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
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES, nullptr, "", 0});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    bool hasMacosGlBinding = false;
    bool hasDisplayInfoExt = false;
    LOG_INFO("Available OpenXR extensions:");
    for (auto &e : exts) {
        LOG_INFO("  %s v%u", e.extensionName, e.extensionVersion);
        if (strcmp(e.extensionName, XR_EXT_MACOS_GL_BINDING_EXTENSION_NAME) == 0)
            hasMacosGlBinding = true;
        if (strcmp(e.extensionName, "XR_EXT_display_info") == 0)
            hasDisplayInfoExt = true;
    }

    if (!hasMacosGlBinding) {
        LOG_ERROR("Runtime does not support XR_EXT_macos_gl_binding");
        return false;
    }

    // Create instance — enable macos_gl_binding + display_info if available
    std::vector<const char*> enabledExts = {XR_EXT_MACOS_GL_BINDING_EXTENSION_NAME};
    if (hasDisplayInfoExt)
        enabledExts.push_back("XR_EXT_display_info");
    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(createInfo.applicationInfo.applicationName, "GLCubeOpenXR",
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

static bool CreateSession(AppXrSession &app, CGLContextObj cglContext, CGLPixelFormatObj cglPixelFormat)
{
    XrGraphicsBindingOpenGLMacOSEXT binding = {};
    binding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_MACOS_EXT;
    binding.cglContext = cglContext;
    binding.cglPixelFormat = cglPixelFormat;

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = app.systemId;

    XR_CHECK(xrCreateSession(app.instance, &sessionInfo, &app.session));
    LOG_INFO("OpenXR session created with CGL binding");

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

    // Query supported formats
    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(app.session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(app.session, formatCount, &formatCount, formats.data()));

    LOG_INFO("Supported swapchain formats:");
    int64_t selectedFormat = formats[0];
    for (auto f : formats) {
        // Prefer GL_RGBA8
        if (f == GL_RGBA8) {
            selectedFormat = f;
        }
    }
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

    // Enumerate swapchain images
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

    LOG_INFO("=== OpenGL Cube OpenXR Test App (macOS, non-ext) ===");

    // Create an offscreen CGL context (GL 4.1 core profile)
    // The runtime creates the window — we just need a GL context for rendering.
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
        kCGLPFADepthSize, (CGLPixelFormatAttribute)24,
        kCGLPFAAccelerated,
        (CGLPixelFormatAttribute)0
    };

    CGLPixelFormatObj pixelFormat = nullptr;
    GLint numFormats = 0;
    CGLError err = CGLChoosePixelFormat(attrs, &pixelFormat, &numFormats);
    if (err != kCGLNoError || !pixelFormat) {
        LOG_ERROR("Failed to create CGL pixel format: %d", err);
        return 1;
    }

    CGLContextObj cglContext = nullptr;
    err = CGLCreateContext(pixelFormat, nullptr, &cglContext);
    if (err != kCGLNoError || !cglContext) {
        LOG_ERROR("Failed to create CGL context: %d", err);
        CGLDestroyPixelFormat(pixelFormat);
        return 1;
    }

    CGLSetCurrentContext(cglContext);
    LOG_INFO("CGL context created (offscreen)");

    // Initialize renderer (needs GL context current)
    GLRenderer renderer = {};
    if (!InitRenderer(renderer)) {
        LOG_ERROR("Failed to initialize OpenGL renderer");
        CGLDestroyContext(cglContext);
        CGLDestroyPixelFormat(pixelFormat);
        return 1;
    }

    // Initialize OpenXR
    AppXrSession app = {};
    if (!InitializeOpenXR(app)) {
        LOG_ERROR("Failed to initialize OpenXR");
        CGLDestroyContext(cglContext);
        CGLDestroyPixelFormat(pixelFormat);
        return 1;
    }

    if (!CreateSession(app, cglContext, pixelFormat)) {
        LOG_ERROR("Failed to create session");
        xrDestroyInstance(app.instance);
        CGLDestroyContext(cglContext);
        CGLDestroyPixelFormat(pixelFormat);
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

        // Render N views into tile positions using runtime-provided tile layout.
        // Falls back to derived layout if mode enumeration unavailable.
        uint32_t tileColumns = (app.currentModeIndex < app.renderingModeCount)
            ? app.renderingModeTileColumns[app.currentModeIndex] : (viewCount >= 2 ? 2 : 1);
        uint32_t tileRows = (app.currentModeIndex < app.renderingModeCount)
            ? app.renderingModeTileRows[app.currentModeIndex] : ((viewCount + tileColumns - 1) / tileColumns);
        if (frameState.shouldRender && viewCount >= 1) {
            uint32_t eyeW = tileColumns > 0 ? app.swapchain.width / tileColumns : app.swapchain.width;
            uint32_t eyeH = tileRows > 0 ? app.swapchain.height / tileRows : app.swapchain.height;

            std::vector<EyeRenderParams> eyeParams(viewCount);
            for (uint32_t i = 0; i < viewCount; i++) {
                uint32_t tileX = i % tileColumns;
                uint32_t tileY = i / tileColumns;
                eyeParams[i].viewportX = tileX * eyeW;
                eyeParams[i].viewportY = tileY * eyeH;
                eyeParams[i].width = eyeW;
                eyeParams[i].height = eyeH;
                mat4_view_from_xr_pose(eyeParams[i].viewMat, views[i].pose);
                mat4_from_xr_fov(eyeParams[i].projMat, views[i].fov, 0.05f, 100.0f);
            }

            RenderScene(renderer, app.swapchain.images[imageIndex],
                        app.swapchain.width, app.swapchain.height, eyeParams.data(), (int)viewCount);
        }

        // Release swapchain image
        XrSwapchainImageReleaseInfo relInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(app.swapchain.swapchain, &relInfo);

        // End frame
        std::vector<XrCompositionLayerProjectionView> projViews(viewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
        for (uint32_t i = 0; i < viewCount; i++) {
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
        projLayer.viewCount = viewCount;
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

    // GL cleanup
    if (renderer.fbo) glDeleteFramebuffers(1, &renderer.fbo);
    if (renderer.depthRBO) glDeleteRenderbuffers(1, &renderer.depthRBO);
    glDeleteVertexArrays(1, &renderer.cubeVAO);
    glDeleteBuffers(1, &renderer.cubeVBO);
    glDeleteBuffers(1, &renderer.cubeEBO);
    glDeleteVertexArrays(1, &renderer.gridVAO);
    glDeleteBuffers(1, &renderer.gridVBO);
    glDeleteTextures(3, renderer.textures);
    glDeleteProgram(renderer.cubeProgram);
    glDeleteProgram(renderer.gridProgram);

    CGLSetCurrentContext(nullptr);
    CGLDestroyContext(cglContext);
    CGLDestroyPixelFormat(pixelFormat);

    LOG_INFO("Clean shutdown complete");
    return 0;
}
