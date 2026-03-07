// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenGL OpenXR spinning cube with IOSurface shared texture (macOS)
 *
 * Demonstrates XR_EXT_macos_gl_binding with IOSurface shared texture:
 * the app creates an IOSurface and passes it to the runtime via the cocoa
 * window binding (viewHandle=NULL, sharedIOSurface=surface). The runtime
 * renders the composited output into the IOSurface. The app then imports
 * the IOSurface as a GL texture via CGLTexImageIOSurface2D and blits it
 * to its own window.
 *
 * Key difference from cube_ext_gl_macos: the app's NSView is NOT passed
 * to the runtime. Instead, the IOSurface acts as a shared render target,
 * and the app composites the result into its own rendering pipeline.
 *
 * Features:
 * - IOSurface shared texture (zero-copy GL texture sharing)
 * - App-owned window with toolbar and status bar UI
 * - Mouse drag camera rotation, scroll zoom, WASD movement
 * - OpenGL rendering (macOS legacy GL 4.1, no Vulkan dependency)
 * - ESC to quit, Space to reset view
 */

#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>
#import <IOSurface/IOSurface.h>

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

// Blit shader — fullscreen triangle sampling IOSurface via GL_TEXTURE_RECTANGLE
static const char *g_blitVertexShader = R"GLSL(
#version 410 core
out vec2 vUV;
uniform vec2 uTexSize;
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    // Flip Y for correct orientation, scale to pixel coords for TEXTURE_RECTANGLE
    vUV = vec2(uv.x, 1.0 - uv.y) * uTexSize;
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
    GLuint blitProgram;

    // Cube uniforms
    GLint cubeLocMVP, cubeLocModel, cubeLocTexSize;
    GLint cubeLocBasecolor, cubeLocNormal, cubeLocAO;

    // Grid uniforms
    GLint gridLocMVP, gridLocColor;

    // Blit uniforms
    GLint blitLocTex, blitLocTexSize;

    // Geometry
    GLuint cubeVAO, cubeVBO, cubeEBO;
    GLuint gridVAO, gridVBO;
    int gridVertexCount;

    // Blit geometry (empty VAO for fullscreen triangle)
    GLuint blitVAO;

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

    // Compile blit shaders
    GLuint blitVS = CompileShader(GL_VERTEX_SHADER, g_blitVertexShader);
    GLuint blitFS = CompileShader(GL_FRAGMENT_SHADER, g_blitFragmentShader);
    if (!blitVS || !blitFS) return false;
    r.blitProgram = LinkProgram(blitVS, blitFS);
    glDeleteShader(blitVS);
    glDeleteShader(blitFS);
    if (!r.blitProgram) return false;

    r.blitLocTex = glGetUniformLocation(r.blitProgram, "uTex");
    r.blitLocTexSize = glGetUniformLocation(r.blitProgram, "uTexSize");

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

    // Blit VAO (empty — fullscreen triangle uses gl_VertexID)
    glGenVertexArrays(1, &r.blitVAO);

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
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, targetTex, 0);
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
// Blit IOSurface GL texture to default framebuffer (letterboxed)
// ============================================================================

static void BlitIOSurfaceToScreen(GLRenderer &r, GLuint ioSurfaceTex,
                                   uint32_t surfW, uint32_t surfH,
                                   uint32_t screenW, uint32_t screenH)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Compute letterbox viewport
    float surfAspect = (float)surfW / (float)surfH;
    float screenAspect = (float)screenW / (float)screenH;
    int vpX, vpY, vpW, vpH;
    if (surfAspect > screenAspect) {
        vpW = (int)screenW;
        vpH = (int)((float)screenW / surfAspect);
        vpX = 0;
        vpY = ((int)screenH - vpH) / 2;
    } else {
        vpH = (int)screenH;
        vpW = (int)((float)screenH * surfAspect);
        vpX = ((int)screenW - vpW) / 2;
        vpY = 0;
    }

    glClearColor(0.08f, 0.08f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);

    glViewport(vpX, vpY, vpW, vpH);

    glUseProgram(r.blitProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_RECTANGLE, ioSurfaceTex);
    glUniform1i(r.blitLocTex, 0);
    glUniform2f(r.blitLocTexSize, (float)surfW, (float)surfH);

    glBindVertexArray(r.blitVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_RECTANGLE, 0);
}

// ============================================================================
// Globals
// ============================================================================

static volatile bool g_running = true;
static NSWindow *g_window = nil;
static NSOpenGLView *g_glView = nil;
static NSOpenGLContext *g_glContext = nil;

// IOSurface shared texture
static IOSurfaceRef g_ioSurface = NULL;
static GLuint g_ioSurfaceGLTex = 0;
static uint32_t g_ioSurfaceWidth = 1920;
static uint32_t g_ioSurfaceHeight = 1080;

// Input state
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

// Current display rendering mode (0=SBS, 1=anaglyph, 2=blend)
static int g_currentOutputMode = 0;

// Performance stats
static double g_avgFrameTime = 0.0;
static float g_hudUpdateTimer = 0.0f;
static uint32_t g_windowW = 1512, g_windowH = 823;
static uint32_t g_renderW = 0, g_renderH = 0;

// UI layout constants
static const float TOOLBAR_HEIGHT = 30.0f;
static const float STATUSBAR_HEIGHT = 30.0f;

static void SignalHandler(int)
{
    g_running = false;
}

// ============================================================================
// HUD overlay (semi-transparent text, rendered as NSView subview)
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
- (NSView *)hitTest:(NSPoint)point { (void)point; return nil; }
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
// Toolbar view (top bar with mode / FPS / info)
// ============================================================================

@interface ToolbarView : NSView
@property (nonatomic, copy) NSString *toolbarText;
@end

@implementation ToolbarView
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) { _toolbarText = @"IOSurface Shared Texture (GL)"; [self setWantsLayer:YES]; }
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
// macOS window creation (NSOpenGLView-backed with UI chrome)
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

        [g_window setTitle:@"OpenGL Cube OpenXR (IOSurface Shared Texture)"];
        [g_window setAcceptsMouseMovedEvents:YES];
        [g_window setReleasedWhenClosed:NO];

        g_windowDelegate = [[AppWindowDelegate alloc] init];
        [g_window setDelegate:g_windowDelegate];

        // Create a container view that holds toolbar + GL view + status bar
        NSView *container = [[NSView alloc] initWithFrame:frame];

        // Toolbar (top)
        NSRect toolbarFrame = NSMakeRect(0, height - TOOLBAR_HEIGHT, width, TOOLBAR_HEIGHT);
        g_toolbarView = [[ToolbarView alloc] initWithFrame:toolbarFrame];
        g_toolbarView.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
        [container addSubview:g_toolbarView];

        // Status bar (bottom)
        NSRect statusFrame = NSMakeRect(0, 0, width, STATUSBAR_HEIGHT);
        g_statusBarView = [[StatusBarView alloc] initWithFrame:statusFrame];
        g_statusBarView.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
        [container addSubview:g_statusBarView];

        // OpenGL view (center, between toolbar and status bar)
        float glH = height - TOOLBAR_HEIGHT - STATUSBAR_HEIGHT;
        NSRect glFrame = NSMakeRect(0, STATUSBAR_HEIGHT, width, glH);

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

        g_glView = [[NSOpenGLView alloc] initWithFrame:glFrame pixelFormat:pixelFormat];
        if (!g_glView) {
            LOG_ERROR("Failed to create NSOpenGLView");
            return false;
        }

        [g_glView setWantsBestResolutionOpenGLSurface:YES];
        g_glView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        g_glContext = [g_glView openGLContext];
        [g_glContext makeCurrentContext];

        [container addSubview:g_glView];

        // HUD overlay (bottom-left of GL view area)
        NSRect hudFrame = NSMakeRect(10, 10, 420, 380);
        g_hudView = [[HudOverlayView alloc] initWithFrame:hudFrame];
        [g_glView addSubview:g_hudView];

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

    if (g_window == nil || g_glView == nil || g_glContext == nil) {
        LOG_ERROR("Failed to create macOS window");
        return false;
    }

    LOG_INFO("Created macOS window (%ux%u) with toolbar + GL view + status bar", width, height);
    return true;
}

// ============================================================================
// IOSurface creation + GL texture import
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

    // Import IOSurface as GL_TEXTURE_RECTANGLE via CGLTexImageIOSurface2D
    glGenTextures(1, &g_ioSurfaceGLTex);
    glBindTexture(GL_TEXTURE_RECTANGLE, g_ioSurfaceGLTex);

    CGLContextObj cglCtx = CGLGetCurrentContext();
    CGLError err = CGLTexImageIOSurface2D(
        cglCtx, GL_TEXTURE_RECTANGLE,
        GL_RGBA8,       // internal format
        width, height,
        GL_BGRA,        // format (matches IOSurface BGRA)
        GL_UNSIGNED_INT_8_8_8_8_REV,  // type
        g_ioSurface, 0  // plane
    );

    if (err != kCGLNoError) {
        LOG_ERROR("CGLTexImageIOSurface2D failed: %d", err);
        glDeleteTextures(1, &g_ioSurfaceGLTex);
        g_ioSurfaceGLTex = 0;
        CFRelease(g_ioSurface);
        g_ioSurface = NULL;
        return false;
    }

    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_RECTANGLE, 0);

    LOG_INFO("Created IOSurface: %ux%u, BGRA8, id=%u, GL tex=%u",
             width, height, IOSurfaceGetID(g_ioSurface), g_ioSurfaceGLTex);
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
                        g_currentOutputMode = ch - '1';
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

        // Update window pixel size (Retina-aware) — use GL view area, not full window
        if (g_glView != nil) {
            NSSize viewSize = [g_glView bounds].size;
            CGFloat backingScale = g_window ? [g_window backingScaleFactor] : 1.0;
            g_windowW = (uint32_t)(viewSize.width * backingScale);
            g_windowH = (uint32_t)(viewSize.height * backingScale);
        }
    }
}

// ============================================================================
// Camera movement
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
    bool supportsDisplayModeSwitch;
    PFN_xrRequestDisplayModeEXT pfnRequestDisplayModeEXT;
    PFN_xrRequestDisplayRenderingModeEXT pfnRequestDisplayRenderingModeEXT;

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
        LOG_ERROR("Runtime does not support XR_EXT_cocoa_window_binding (required for IOSurface mode)");
        return false;
    }
    LOG_INFO("XR_EXT_display_info: %s", app.hasDisplayInfoExt ? "available" : "not available");

    // Enable extensions
    std::vector<const char *> enabledExts = {
        XR_EXT_MACOS_GL_BINDING_EXTENSION_NAME,
        XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME
    };
    if (hasOpenGLEnable) {
        enabledExts.push_back(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
    }
    if (app.hasDisplayInfoExt) {
        enabledExts.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);
    }

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(createInfo.applicationInfo.applicationName, "GLCubeSharedTexture",
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
                app.supportsDisplayModeSwitch = displayInfo.supportsDisplayModeSwitch;
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
    LOG_INFO("Creating OpenXR session with IOSurface shared texture (GL)...");

    // Get CGL context from our NSOpenGLContext
    CGLContextObj cglCtx = (CGLContextObj)[g_glContext CGLContextObj];

    XrGraphicsBindingOpenGLMacOSEXT glBinding = {};
    glBinding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_MACOS_EXT;
    glBinding.cglContext = (void *)cglCtx;
    glBinding.cglPixelFormat = nullptr;

    // Chain the cocoa window binding:
    // viewHandle=NULL (offscreen), sharedIOSurface=our IOSurface
    XrCocoaWindowBindingCreateInfoEXT cocoaBinding = {};
    cocoaBinding.type = XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT;
    cocoaBinding.next = nullptr;
    cocoaBinding.viewHandle = NULL;  // Offscreen — no NSView passed to runtime
    cocoaBinding.sharedIOSurface = (void *)g_ioSurface;

    glBinding.next = &cocoaBinding;
    LOG_INFO("Chaining XR_EXT_cocoa_window_binding: viewHandle=NULL, sharedIOSurface=%p", (void *)g_ioSurface);

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &glBinding;
    sessionInfo.systemId = app.systemId;

    XR_CHECK(xrCreateSession(app.instance, &sessionInfo, &app.session));
    LOG_INFO("Session created (IOSurface shared texture mode, GL)");

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
        // Prefer GL_RGBA8 (0x8058)
        if (f == 0x8058) {
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

    LOG_INFO("=== OpenGL Cube OpenXR (IOSurface Shared Texture) ===");

    // Create the macOS window with OpenGL context (app-owned, with UI chrome)
    if (!CreateMacOSWindow(1512, 883)) {  // Extra height for UI chrome
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

    // Initialize OpenXR (needed before IOSurface creation to know display dimensions)
    AppXrSession app = {};
    if (!InitializeOpenXR(app)) {
        LOG_ERROR("Failed to initialize OpenXR");
        return 1;
    }

    if (!GetGLGraphicsRequirements(app)) {
        LOG_ERROR("Failed to get OpenGL graphics requirements");
        return 1;
    }

    // Determine IOSurface size from display pixel dimensions or recommended swapchain
    uint32_t ioW = app.displayPixelWidth > 0 ? app.displayPixelWidth : 1920;
    uint32_t ioH = app.displayPixelHeight > 0 ? app.displayPixelHeight : 1080;
    LOG_INFO("IOSurface dimensions: %ux%u", ioW, ioH);

    // Create the shared IOSurface and import as GL texture
    [g_glContext makeCurrentContext];
    if (!CreateIOSurface(ioW, ioH)) {
        LOG_ERROR("Failed to create IOSurface");
        return 1;
    }

    // Create session with IOSurface (viewHandle=NULL)
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
                LOG_INFO("Display mode -> %s (%s)",
                    g_input.displayMode3D ? "3D" : "2D",
                    XR_SUCCEEDED(modeResult) ? "OK" : "failed");
            }
        }

        // Handle rendering mode change (1/2/3 keys)
        if (g_input.renderingModeChangeRequested) {
            g_input.renderingModeChangeRequested = false;
            if (app.pfnRequestDisplayRenderingModeEXT && app.session != XR_NULL_HANDLE) {
                const char *modeNames[] = {"SBS", "Anaglyph", "Blend"};
                XrResult res = app.pfnRequestDisplayRenderingModeEXT(app.session, (uint32_t)g_currentOutputMode);
                LOG_INFO("Rendering mode -> %s (%s)",
                    modeNames[g_currentOutputMode],
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

        XrCompositionLayerProjectionView projViews[2] = {};
        bool rendered = false;

        // Render
        if (frameState.shouldRender && viewCount >= 2) {
            XrVector3f rawEyePos[2] = {views[0].pose.position, views[1].pose.position};
            app.leftEyeX = rawEyePos[0].x; app.leftEyeY = rawEyePos[0].y; app.leftEyeZ = rawEyePos[0].z;
            app.rightEyeX = rawEyePos[1].x; app.rightEyeY = rawEyePos[1].y; app.rightEyeZ = rawEyePos[1].z;

            int eyeCount = g_input.displayMode3D ? 2 : 1;

            if (!g_input.displayMode3D) {
                rawEyePos[0] = {
                    (rawEyePos[0].x + rawEyePos[1].x) / 2.0f,
                    (rawEyePos[0].y + rawEyePos[1].y) / 2.0f,
                    (rawEyePos[0].z + rawEyePos[1].z) / 2.0f};
                rawEyePos[1] = rawEyePos[0];
            }

            XrPosef cameraPose;
            quat_from_yaw_pitch(g_input.yaw, g_input.pitch, &cameraPose.orientation);
            cameraPose.position = {g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ};

            XrVector3f nominalViewer = {app.nominalViewerX, app.nominalViewerY, app.nominalViewerZ};

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

            // Compute stereo views using display3d or camera3d
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

                // display3d/camera3d already produce OpenGL-convention z[-1,1] — no conversion needed
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

            RenderScene(renderer, app.swapchain.images[imageIndex],
                        app.swapchain.width, app.swapchain.height,
                        eyeParams, eyeCount);
        }

        // Release swapchain image
        XrSwapchainImageReleaseInfo relInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(app.swapchain.swapchain, &relInfo);

        // End frame — this triggers the compositor to render into the IOSurface
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

        // Blit the IOSurface content into the app's own GL view
        if (g_ioSurfaceGLTex != 0) {
            BlitIOSurfaceToScreen(renderer, g_ioSurfaceGLTex,
                                   g_ioSurfaceWidth, g_ioSurfaceHeight,
                                   g_windowW, g_windowH);
            [g_glContext flushBuffer];
        }

        // FPS tracking
        g_avgFrameTime = g_avgFrameTime * 0.95 + dt * 0.05;

        // Update UI (throttled)
        g_hudUpdateTimer += dt;
        if (g_hudUpdateTimer >= 0.5f) {
            g_hudUpdateTimer = 0.0f;
            @autoreleasepool {
                double fps = (g_avgFrameTime > 0) ? 1.0 / g_avgFrameTime : 0;
                const char *outputModeNames[] = {"SBS", "Anaglyph", "Blend"};
                const char *outputModeName = (g_currentOutputMode >= 0 && g_currentOutputMode <= 2)
                    ? outputModeNames[g_currentOutputMode] : "?";

                // Update toolbar
                if (g_toolbarView != nil) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        g_toolbarView.toolbarText = [NSString stringWithFormat:
                            @"IOSurface Shared Texture (GL) | %s | Output: %s | FPS: %.0f (%.1fms) | IOSurf: %ux%u",
                            g_input.displayMode3D ? "3D" : "2D",
                            outputModeName, fps, g_avgFrameTime * 1000.0,
                            g_ioSurfaceWidth, g_ioSurfaceHeight];
                        [g_toolbarView setNeedsDisplay:YES];
                    });
                }

                // Update status bar
                if (g_statusBarView != nil) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        const char *modeLabel = g_input.cameraMode ? "Camera" : "Display";
                        g_statusBarView.statusText = [NSString stringWithFormat:
                            @"Eye L:(%.3f,%.3f,%.3f) R:(%.3f,%.3f,%.3f) | %s:(%.2f,%.2f,%.2f) | IPD:%.2f Par:%.2f",
                            app.leftEyeX, app.leftEyeY, app.leftEyeZ,
                            app.rightEyeX, app.rightEyeY, app.rightEyeZ,
                            modeLabel,
                            g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ,
                            g_input.stereo.ipdFactor, g_input.stereo.parallaxFactor];
                        [g_statusBarView setNeedsDisplay:YES];
                    });
                }

                // Update HUD overlay
                if (g_input.hudVisible && g_hudView != nil) {
                    const char *poseLabel = g_input.cameraMode ? "Virtual Camera" : "Virtual Display";
                    const char *param1Label = g_input.cameraMode ? "Conv" : "Persp";
                    const char *param2Label = g_input.cameraMode ? "Zoom" : "Scale";
                    const char *kooimaMode = g_input.cameraMode ? "Camera-Centric [C=Toggle]" : "Display-Centric [C=Toggle]";
                    const char *scrollHint = g_input.cameraMode ? "Scroll=Zoom" : "Scroll=Scale";
                    const char *perspHint = g_input.cameraMode ? "Opt=Conv" : "Opt=Persp";

                    const char *sessionStateNames[] = {
                        "UNKNOWN", "IDLE", "READY", "SYNCHRONIZED",
                        "VISIBLE", "FOCUSED", "STOPPING", "LOSS_PENDING", "EXITING"};
                    int stateIdx = (int)app.sessionState;
                    const char *sessionStateName = (stateIdx >= 0 && stateIdx < 9)
                        ? sessionStateNames[stateIdx] : "INVALID";

                    NSString *valueLineStr;
                    if (g_input.cameraMode) {
                        float tanHFOV = CAMERA_HALF_TAN_VFOV / g_input.stereo.zoomFactor;
                        valueLineStr = [NSString stringWithFormat:@"tanHFOV: %.3f", tanHFOV];
                    } else {
                        float m2v = (g_input.stereo.virtualDisplayHeight > 0.0f && app.displayHeightM > 0.0f)
                            ? g_input.stereo.virtualDisplayHeight / app.displayHeightM : 1.0f;
                        valueLineStr = [NSString stringWithFormat:@"vHeight: %.3f  m2v: %.3f",
                            g_input.stereo.virtualDisplayHeight, m2v];
                    }

                    dispatch_async(dispatch_get_main_queue(), ^{
                        NSString *text = [NSString stringWithFormat:
                            @"%s (OpenGL, IOSurface)\n"
                            "Session: %s\n"
                            "Kooima: %s\n"
                            "Render: %ux%u  Window: %ux%u\n"
                            "Display: %.3f x %.3f m\n"
                            "Nominal: (%.3f, %.3f, %.3f)\n"
                            "%s: (%.2f, %.2f, %.2f)\n"
                            "IPD: %.2f  Parallax: %.2f\n"
                            "%s: %.2f  %s: %.2f\n"
                            "%@\n"
                            "\n"
                            "WASD/QE=Move  Drag=Look  Space=Reset\n"
                            "%s  Shift=IPD  Ctrl=Parallax  %s\n"
                            "C=Mode  V=2D/3D  1/2/3=Output  Tab=HUD  ESC=Quit",
                            app.systemName,
                            sessionStateName,
                            kooimaMode,
                            g_renderW, g_renderH,
                            g_windowW, g_windowH,
                            app.displayWidthM, app.displayHeightM,
                            app.nominalViewerX, app.nominalViewerY, app.nominalViewerZ,
                            poseLabel,
                            g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ,
                            g_input.stereo.ipdFactor, g_input.stereo.parallaxFactor,
                            param1Label, g_input.cameraMode ? g_input.stereo.invConvergenceDistance : g_input.stereo.perspectiveFactor,
                            param2Label, g_input.cameraMode ? g_input.stereo.zoomFactor : g_input.stereo.scaleFactor,
                            valueLineStr,
                            scrollHint, perspHint];
                        g_hudView.hudText = text;
                        [g_hudView setNeedsDisplay:YES];
                        [g_hudView setHidden:NO];
                    });
                } else if (g_hudView != nil) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [g_hudView setHidden:YES];
                    });
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

    // Release IOSurface resources
    if (g_ioSurfaceGLTex != 0) {
        glDeleteTextures(1, &g_ioSurfaceGLTex);
        g_ioSurfaceGLTex = 0;
    }
    if (g_ioSurface != NULL) {
        CFRelease(g_ioSurface);
        g_ioSurface = NULL;
    }

    LOG_INFO("Clean shutdown complete");
    return 0;
}
