// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenGL OpenXR spinning cube with external window (macos_gl_binding)
 *
 * Demonstrates XR_EXT_macos_gl_binding with OpenGL: the app creates its own
 * NSWindow + NSOpenGLView and passes the CGL context to the runtime.
 * The runtime routes GL rendering through the Metal native compositor
 * using IOSurface-backed GL_TEXTURE_RECTANGLE textures.
 *
 * Features:
 * - App creates and owns the NSWindow (XR_EXT_cocoa_window_binding)
 * - Mouse drag camera rotation, scroll zoom, WASD movement
 * - OpenGL rendering (macOS legacy GL 4.1, no Metal/Vulkan)
 * - ESC to quit, Space to reset view
 */

#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>

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

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "display3d_view.h"
#include "camera3d_view.h"
#include "view_params.h"
#include "atlas_capture.h"
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
// Globals
// ============================================================================

static volatile bool g_running = true;
static NSWindow *g_window = nil;
static NSOpenGLView *g_glView = nil;
static NSOpenGLContext *g_glContext = nil;

// Input state
struct InputState {
    float yaw = 0.0f, pitch = 0.0f;
    bool keyW = false, keyA = false, keyS = false, keyD = false;
    bool keyE = false, keyQ = false;
    float cameraPosX = 0.0f, cameraPosY = 0.0f, cameraPosZ = 0.0f;
    bool resetViewRequested = false;
    ViewParams viewParams;
    bool hudVisible = true;
    uint32_t currentRenderingMode = 1;
    uint32_t renderingModeCount = 0;
    bool renderingModeChangeRequested = false;
    bool cameraMode = false;
    float nominalViewerZ = 0.5f;
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
// macOS window creation (NSOpenGLView-backed)
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

        [g_window setTitle:@"OpenGL Cube — Metal Native Compositor (External Window)"];
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

        // HUD overlay
        NSRect hudFrame = NSMakeRect(10, 10, 420, 380);
        g_hudView = [[HudOverlayView alloc] initWithFrame:hudFrame];
        [g_glView addSubview:g_hudView];

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
                    else if (ch == '\t' && !isRepeat) { g_input.hudVisible = !g_input.hudVisible; }
                    else if (ch == 'v' && !isRepeat) {
                        if (g_input.renderingModeCount > 0) {
                            g_input.currentRenderingMode = (g_input.currentRenderingMode + 1) % g_input.renderingModeCount;
                        }
                        g_input.renderingModeChangeRequested = true;
                    }
                    else if ((ch == 'i' || ch == 'I') && !isRepeat) {
                        g_input.captureAtlasRequested = true;
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
                        uint32_t idx = ch - '0';
                        if (idx < g_input.renderingModeCount) {
                            g_input.currentRenderingMode = idx;
                            g_input.renderingModeChangeRequested = true;
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
    std::vector<GLuint> images; // GL texture names (GL_TEXTURE_RECTANGLE)
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
    bool hasMacosGlBinding;

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
    uint32_t renderingModeCount;
    char renderingModeNames[8][XR_MAX_SYSTEM_NAME_SIZE];
    uint32_t renderingModeViewCounts[8] = {};
    uint32_t renderingModeTileColumns[8] = {};
    uint32_t renderingModeTileRows[8] = {};
    float renderingModeScaleX[8] = {};
    float renderingModeScaleY[8] = {};
    bool renderingModeDisplay3D[8] = {};

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

    bool hasOpenGLEnable = false;
    app.hasCocoaWindowBinding = false;
    app.hasMacosGlBinding = false;
    app.hasDisplayInfoExt = false;

    LOG_INFO("Available OpenXR extensions:");
    for (auto &e : exts) {
        LOG_INFO("  %s v%u", e.extensionName, e.extensionVersion);
        if (strcmp(e.extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0)
            hasOpenGLEnable = true;
        if (strcmp(e.extensionName, XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME) == 0)
            app.hasCocoaWindowBinding = true;
        if (strcmp(e.extensionName, XR_EXT_MACOS_GL_BINDING_EXTENSION_NAME) == 0)
            app.hasMacosGlBinding = true;
        if (strcmp(e.extensionName, XR_EXT_DISPLAY_INFO_EXTENSION_NAME) == 0)
            app.hasDisplayInfoExt = true;
    }

    if (!app.hasMacosGlBinding) {
        LOG_ERROR("Runtime does not support XR_EXT_macos_gl_binding");
        return false;
    }
    if (!app.hasCocoaWindowBinding) {
        LOG_WARN("Runtime does not support XR_EXT_cocoa_window_binding — will create own window");
    }
    LOG_INFO("XR_EXT_display_info: %s", app.hasDisplayInfoExt ? "available" : "not available");

    // Enable extensions
    std::vector<const char *> enabledExts = {XR_EXT_MACOS_GL_BINDING_EXTENSION_NAME};
    if (hasOpenGLEnable) {
        enabledExts.push_back(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
    }
    if (app.hasCocoaWindowBinding) {
        enabledExts.push_back(XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME);
    }
    if (app.hasDisplayInfoExt) {
        enabledExts.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);
    }

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(createInfo.applicationInfo.applicationName, "GLCubeExtOpenXR",
            XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.applicationVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = (uint32_t)enabledExts.size();
    createInfo.enabledExtensionNames = enabledExts.data();

    XR_CHECK(xrCreateInstance(&createInfo, &app.instance));
    LOG_INFO("OpenXR instance created");
    LOG_INFO("XR_EXT_macos_gl_binding: enabled");
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

static bool CreateSession(AppXrSession &app)
{
    LOG_INFO("Creating OpenXR session with macOS GL binding + cocoa_window_binding...");

    // Get CGL context from our NSOpenGLContext
    CGLContextObj cglCtx = (CGLContextObj)[g_glContext CGLContextObj];

    XrGraphicsBindingOpenGLMacOSEXT glBinding = {};
    glBinding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_MACOS_EXT;
    glBinding.cglContext = (void *)cglCtx;
    glBinding.cglPixelFormat = nullptr;

    // Chain the cocoa window binding — pass our NSView to the runtime
    XrCocoaWindowBindingCreateInfoEXT cocoaBinding = {};
    cocoaBinding.type = XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT;
    cocoaBinding.next = nullptr;
    cocoaBinding.viewHandle = (__bridge void *)g_glView;

    if (app.hasCocoaWindowBinding) {
        glBinding.next = &cocoaBinding;
        LOG_INFO("Chaining XR_EXT_cocoa_window_binding with NSView %p", cocoaBinding.viewHandle);
    }

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &glBinding;
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
                    app.renderingModeViewCounts[i] = modes[i].viewCount;
                    app.renderingModeTileColumns[i] = modes[i].tileColumns;
                    app.renderingModeTileRows[i] = modes[i].tileRows;
                    app.renderingModeScaleX[i] = modes[i].viewScaleX;
                    app.renderingModeScaleY[i] = modes[i].viewScaleY;
                    app.renderingModeDisplay3D[i] = modes[i].hardwareDisplay3D ? true : false;
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

    LOG_INFO("Supported swapchain formats:");
    int64_t selectedFormat = formats[0];
    for (auto f : formats) {
        // Prefer GL_RGBA8 (0x8058) or GL_SRGB8_ALPHA8 (0x8C43)
        if (f == 0x8058) { // GL_RGBA8
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

    LOG_INFO("=== OpenGL Cube OpenXR (External Window, macOS) ===");

    // Create the macOS window with OpenGL context (app-owned)
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

    if (!CreateSession(app)) {
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

    // Initialize output mode from env var
    {
        const char *mode_str = getenv("SIM_DISPLAY_OUTPUT");
        if (mode_str) {
            if (strcmp(mode_str, "anaglyph") == 0)
                g_input.currentRenderingMode = 1;
            else if (strcmp(mode_str, "sbs") == 0)
                g_input.currentRenderingMode = 2;
            else if (strcmp(mode_str, "blend") == 0)
                g_input.currentRenderingMode = 3;
            else
                g_input.currentRenderingMode = 1; // default to anaglyph
        }
    }

    g_input.renderingModeChangeRequested = true;

    g_input.viewParams.virtualDisplayHeight = 0.24f;
    g_input.nominalViewerZ = app.nominalViewerZ;

    LOG_INFO("Entering main loop... (ESC to quit, drag to rotate, WASD to move, Space to reset)");
    LOG_INFO("Controls: WASD/QE=Move, Drag=Look, Scroll=Scale, Space=Reset, V=Mode, Tab=HUD, ESC=Quit");

    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !app.exitRequested) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        PumpMacOSEvents();
        PollEvents(app);

        // Handle rendering mode change (V=cycle, 0-8=direct)
        if (g_input.renderingModeChangeRequested) {
            g_input.renderingModeChangeRequested = false;
            if (app.pfnRequestDisplayRenderingModeEXT && app.session != XR_NULL_HANDLE) {
                const char *modeName = (g_input.currentRenderingMode < app.renderingModeCount)
                    ? app.renderingModeNames[g_input.currentRenderingMode] : "?";
                XrResult res = app.pfnRequestDisplayRenderingModeEXT(app.session, g_input.currentRenderingMode);
                LOG_INFO("Rendering mode -> %s (%s)",
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

        // Make our GL context current for rendering
        [g_glContext makeCurrentContext];

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

        bool rendered = false;
        bool display3D = (g_input.currentRenderingMode < app.renderingModeCount)
            ? app.renderingModeDisplay3D[g_input.currentRenderingMode] : true;

        // Get N-view mode info from enumerated rendering modes
        uint32_t modeViewCount = (g_input.currentRenderingMode < app.renderingModeCount)
            ? app.renderingModeViewCounts[g_input.currentRenderingMode] : 2;
        uint32_t tileColumns = (g_input.currentRenderingMode < app.renderingModeCount)
            ? app.renderingModeTileColumns[g_input.currentRenderingMode] : 2;
        uint32_t tileRows = (g_input.currentRenderingMode < app.renderingModeCount)
            ? app.renderingModeTileRows[g_input.currentRenderingMode] : 1;
        int eyeCount = display3D ? (int)modeViewCount : 1;

        // Dynamic arrays for N-view rendering
        std::vector<XrCompositionLayerProjectionView> projViews(eyeCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        // Render
        if (frameState.shouldRender && viewCount >= 1) {
            // Save raw display-space eye positions for Kooima + HUD
            app.eyeCount = modeViewCount;
            for (uint32_t v = 0; v < viewCount && v < 8; v++) {
                app.eyePositions[v][0] = views[v].pose.position.x;
                app.eyePositions[v][1] = views[v].pose.position.y;
                app.eyePositions[v][2] = views[v].pose.position.z;
            }
            std::vector<XrVector3f> rawEyePos(modeViewCount);
            for (uint32_t v = 0; v < modeViewCount; v++) {
                rawEyePos[v] = (v < viewCount) ? views[v].pose.position : views[0].pose.position;
            }

            // In mono mode, use center eye
            if (!display3D && modeViewCount >= 2) {
                XrVector3f center = {0, 0, 0};
                for (uint32_t v = 0; v < modeViewCount; v++) {
                    center.x += rawEyePos[v].x;
                    center.y += rawEyePos[v].y;
                    center.z += rawEyePos[v].z;
                }
                center.x /= modeViewCount;
                center.y /= modeViewCount;
                center.z /= modeViewCount;
                rawEyePos[0] = center;
            }

            XrPosef cameraPose;
            quat_from_yaw_pitch(g_input.yaw, g_input.pitch, &cameraPose.orientation);
            cameraPose.position = {g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ};

            XrVector3f nominalViewer = {app.nominalViewerX, app.nominalViewerY, app.nominalViewerZ};

            float scaleX = (g_input.currentRenderingMode < app.renderingModeCount)
                ? app.renderingModeScaleX[g_input.currentRenderingMode] : 0.5f;
            float scaleY = (g_input.currentRenderingMode < app.renderingModeCount)
                ? app.renderingModeScaleY[g_input.currentRenderingMode] : 0.5f;
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

            // Compute N views using display3d or camera3d library
            std::vector<Display3DView> d3dViews(eyeCount);
            bool hasKooima = (app.displayWidthM > 0 && app.displayHeightM > 0);
            if (hasKooima) {
                float dispPxW = app.displayPixelWidth > 0 ? (float)app.displayPixelWidth : (float)app.swapchain.width;
                float dispPxH = app.displayPixelHeight > 0 ? (float)app.displayPixelHeight : (float)app.swapchain.height;
                float pxSizeX = app.displayWidthM / dispPxW;
                float pxSizeY = app.displayHeightM / dispPxH;
                float winW_m = (float)g_windowW * pxSizeX;
                float winH_m = (float)g_windowH * pxSizeY;

                // Window-relative Kooima: compute eye offset from window center
                float eyeOffsetX = 0.0f, eyeOffsetY = 0.0f;
                if (g_window != nil) {
                    NSRect winFrame = [g_window frame];
                    NSScreen *screen_ns = [g_window screen] ?: [NSScreen mainScreen];
                    NSRect screenFrame = [screen_ns frame];
                    float winCenterX = (winFrame.origin.x - screenFrame.origin.x) + winFrame.size.width / 2.0f;
                    float winCenterY = (winFrame.origin.y - screenFrame.origin.y) + winFrame.size.height / 2.0f;
                    float dispCenterX = screenFrame.size.width / 2.0f;
                    float dispCenterY = screenFrame.size.height / 2.0f;
                    CGFloat backingScale = [g_window backingScaleFactor];
                    float pxSizeXBacking = pxSizeX / (float)backingScale;
                    float pxSizeYBacking = pxSizeY / (float)backingScale;
                    eyeOffsetX = (winCenterX - dispCenterX) * pxSizeXBacking;
                    eyeOffsetY = (winCenterY - dispCenterY) * pxSizeYBacking;
                }

                Display3DScreen screen;
                screen.width_m = winW_m;
                screen.height_m = winH_m;

                // Apply window center offset to raw eye positions
                for (uint32_t v = 0; v < modeViewCount; v++) {
                    rawEyePos[v].x -= eyeOffsetX;
                    rawEyePos[v].y -= eyeOffsetY;
                }

                if (g_input.cameraMode) {
                    Camera3DTunables camTunables;
                    camTunables.ipd_factor = g_input.viewParams.ipdFactor;
                    camTunables.parallax_factor = g_input.viewParams.parallaxFactor;
                    camTunables.inv_convergence_distance = g_input.viewParams.invConvergenceDistance;
                    camTunables.half_tan_vfov = CAMERA_HALF_TAN_VFOV / g_input.viewParams.zoomFactor;

                    std::vector<Camera3DView> camViews(eyeCount);
                    camera3d_compute_views(
                        rawEyePos.data(), eyeCount, &nominalViewer,
                        &screen, &camTunables, &cameraPose,
                        0.01f, 100.0f, camViews.data());

                    for (int i = 0; i < eyeCount; i++) {
                        memcpy(d3dViews[i].view_matrix, camViews[i].view_matrix, sizeof(float) * 16);
                        memcpy(d3dViews[i].projection_matrix, camViews[i].projection_matrix, sizeof(float) * 16);
                        d3dViews[i].fov = camViews[i].fov;
                        d3dViews[i].eye_world = camViews[i].eye_world;
                    }
                } else {
                    Display3DTunables tunables;
                    tunables.ipd_factor = g_input.viewParams.ipdFactor;
                    tunables.parallax_factor = g_input.viewParams.parallaxFactor;
                    tunables.perspective_factor = g_input.viewParams.perspectiveFactor;
                    tunables.virtual_display_height = g_input.viewParams.virtualDisplayHeight / g_input.viewParams.scaleFactor;

                    display3d_compute_views(
                        rawEyePos.data(), eyeCount, &nominalViewer,
                        &screen, &tunables, &cameraPose,
                        0.01f, 100.0f, d3dViews.data());
                }

                // display3d/camera3d already produce OpenGL-convention z[-1,1] — no conversion needed
            }

            rendered = true;
            std::vector<EyeRenderParams> eyeParams(eyeCount);
            for (int eye = 0; eye < eyeCount; eye++) {
                XrFovf submitFov = views[eye < (int)viewCount ? eye : 0].fov;
                if (hasKooima) {
                    memcpy(eyeParams[eye].viewMat, d3dViews[eye].view_matrix, sizeof(float) * 16);
                    memcpy(eyeParams[eye].projMat, d3dViews[eye].projection_matrix, sizeof(float) * 16);
                    submitFov = d3dViews[eye].fov;
                    views[eye < (int)viewCount ? eye : 0].pose.position = d3dViews[eye].eye_world;
                    views[eye < (int)viewCount ? eye : 0].pose.orientation = cameraPose.orientation;
                } else {
                    int vi = eye < (int)viewCount ? eye : 0;
                    mat4_view_from_xr_pose(eyeParams[eye].viewMat, views[vi].pose);
                    mat4_from_xr_fov(eyeParams[eye].projMat, views[vi].fov, 0.01f, 100.0f);
                }

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
                        app.swapchain.width, app.swapchain.height,
                        eyeParams.data(), eyeCount);

            // 'I' key: snapshot the multi-view atlas. Skipped for mono.
            if (g_input.captureAtlasRequested) {
                g_input.captureAtlasRequested = false;
                if (display3D && (tileColumns > 1 || tileRows > 1)) {
                    uint32_t atlasW = tileColumns * renderW;
                    uint32_t atlasH = tileRows * renderH;
                    if (atlasW <= app.swapchain.width && atlasH <= app.swapchain.height) {
                        std::string outPath = dxr_capture::MakeCapturePath(
                            "cube_handle_gl_macos", tileColumns, tileRows);
                        bool ok = dxr_capture::CaptureAtlasRegionGL(
                            (uint32_t)app.swapchain.images[imageIndex],
                            app.swapchain.width, app.swapchain.height,
                            0, 0, atlasW, atlasH, outPath);
                        if (ok) {
                            LOG_INFO("Captured atlas %ux%u -> %s",
                                     atlasW, atlasH, outPath.c_str());
                            dxr_capture::TriggerCaptureFlash((__bridge void*)g_glView);
                        }
                    }
                } else {
                    LOG_WARN("Capture skipped: need 3D mode with cols/rows > 1");
                }
            }
        }

        // Release swapchain image
        XrSwapchainImageReleaseInfo relInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(app.swapchain.swapchain, &relInfo);

        // End frame
        {
            XrCompositionLayerProjection projLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projLayer.space = app.localSpace;
            projLayer.viewCount = (uint32_t)eyeCount;
            projLayer.views = projViews.data();

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
                        float tanHFOV = CAMERA_HALF_TAN_VFOV / g_input.viewParams.zoomFactor;
                        snprintf(valueLine, sizeof(valueLine), "tanHFOV: %.3f", tanHFOV);
                    } else {
                        float m2v = (g_input.viewParams.virtualDisplayHeight > 0.0f && app.displayHeightM > 0.0f)
                            ? g_input.viewParams.virtualDisplayHeight / app.displayHeightM : 1.0f;
                        snprintf(valueLine, sizeof(valueLine), "vHeight: %.3f  m2v: %.3f",
                            g_input.viewParams.virtualDisplayHeight, m2v);
                    }

                    const char *outputModeName = (g_input.currentRenderingMode < app.renderingModeCount)
                        ? app.renderingModeNames[g_input.currentRenderingMode] : "?";

                    // Build output mode hint: "1-N=Output" if >1 mode, empty if single
                    NSString *outputHintStr = @"";
                    if (app.renderingModeCount > 1) {
                        outputHintStr = [NSString stringWithFormat:@"  0-%u=Mode", app.renderingModeCount - 1];
                    }

                    // Build dynamic eye position lines based on active view count
                    NSMutableString *eyeLines = [NSMutableString string];
                    for (uint32_t e = 0; e < app.eyeCount && e < 8; e++) {
                        [eyeLines appendFormat:@"Eye[%u]: (%.3f, %.3f, %.3f)\n",
                            e, app.eyePositions[e][0], app.eyePositions[e][1], app.eyePositions[e][2]];
                    }

                    NSString *text = [NSString stringWithFormat:
                        @"%s (OpenGL)\n"
                        "Session: %s\n"
                        "Mode: %s (%s)\n"
                        "Kooima: %s\n"
                        "FPS: %.0f  (%.1f ms)\n"
                        "Render: %ux%u  Window: %ux%u\n"
                        "Display: %.3f x %.3f m\n"
                        "Scale: %.2f x %.2f\n"
                        "Nominal: (%.3f, %.3f, %.3f)\n"
                        "%@"
                        "%s: (%.2f, %.2f, %.2f)\n"
                        "IPD: %.2f  Parallax: %.2f\n"
                        "%s: %.2f  %s: %.2f\n"
                        "%s\n"
                        "\n"
                        "WASD/QE=Move  Drag=Look  Space=Reset\n"
                        "%s  Shift=IPD  Ctrl=Parallax  %s\n"
                        "V=Mode%@  Tab=HUD  ESC=Quit",
                        app.systemName,
                        sessionStateName,
                        outputModeName,
                        display3D ? "3D" : "2D",
                        kooimaMode,
                        fps, g_avgFrameTime * 1000.0,
                        g_renderW, g_renderH,
                        g_windowW, g_windowH,
                        app.displayWidthM, app.displayHeightM,
                        app.recommendedViewScaleX, app.recommendedViewScaleY,
                        app.nominalViewerX, app.nominalViewerY, app.nominalViewerZ,
                        eyeLines,
                        poseLabel,
                        g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ,
                        g_input.viewParams.ipdFactor, g_input.viewParams.parallaxFactor,
                        param1Label, g_input.cameraMode ? g_input.viewParams.invConvergenceDistance : g_input.viewParams.perspectiveFactor,
                        param2Label, g_input.cameraMode ? g_input.viewParams.zoomFactor : g_input.viewParams.scaleFactor,
                        valueLine,
                        scrollHint, perspHint,
                        outputHintStr];
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
