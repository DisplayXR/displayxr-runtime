// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenGL OpenXR spinning cube with external window (macos_gl_binding)
 *
 * Demonstrates XR_DXR_macos_gl_binding with OpenGL: the app creates its own
 * NSWindow + NSOpenGLView and passes the CGL context to the runtime.
 * The runtime routes GL rendering through the Metal native compositor
 * using IOSurface-backed GL_TEXTURE_RECTANGLE textures.
 *
 * Features:
 * - App creates and owns the NSWindow (XR_DXR_cocoa_window_binding)
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
#include <openxr/XR_DXR_cocoa_window_binding.h>
#include <openxr/XR_DXR_macos_gl_binding.h>

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
#include "mode_switch.h" // dxr::ModeSwitch — smooth 2D<->3D disparity ramp (inline on macOS)
#include "rig_mode.h"
#include "atlas_capture.h"
#include "xr_window_space_hud.h"
#include "hud_renderer_macos.h"
#include <openxr/XR_DXR_display_info.h>
#include <openxr/XR_DXR_atlas_capture.h>
#include <openxr/XR_DXR_view_rig.h>
#include "dxr_view_config.h" // #1486 PRIMARY_MULTIVIEW_DXR opt-in

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
    // Rendering mode REQUESTS — single source of truth lives on the runtime
    // side (read back as app.currentModeIndex after the runtime's
    // XrEventDataRenderingModeChangedDXR lands). Keys emit transient requests;
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
// Cube test apps start with the WSUI HUD hidden — it skews perf comparisons
// against HUD-less apps (avatar, Unity). Shift+Tab shows it when wanted.
static InputState g_input = [] { InputState s; s.hudVisible = false; return s; }();
// Smooth 2D<->3D disparity ramp, driven inline (macOS has its own AppXrSession/
// InputState, not the Windows-only XrSessionManager helper).
static dxr::ModeSwitch g_modeSwitch;
static bool g_modeSwitchConfigured = false;
static const float CAMERA_HALF_TAN_VFOV = 0.32491969623f; // tan(18deg) -> 36deg vFOV

// HUD window-space layer (XR_EXT_window_space_layer): replaces the legacy
// NSView overlay so the runtime composes the HUD via the proper extension
// path on macOS. Same constants as the other macOS test apps.
static const uint32_t HUD_PIXEL_WIDTH = 380;
static const uint32_t HUD_PIXEL_HEIGHT = 470;
static const float HUD_WIDTH_FRACTION = 0.20f;

// #1581 — DXR_TEST_QUAD=1 submits real XrCompositionLayerQuad layers so the
// GL quad pass can be judged on pixels. Default OFF: without it this app's
// layer list is byte-identical to before.
//
// Three quads, all sharing one deliberately NON-symmetric 256x256 probe
// texture (coloured checker, a large "Q" with its tail bottom-right, four
// distinct corner blocks, an 8 px white border, and a half-alpha quadrant):
//   (A) LOCAL space, axis-aligned, BOTH eyes, STRAIGHT alpha — the
//       measurement / alpha / stereo-disparity quad.
//   (B) LOCAL space, yawed -15 deg about up, BOTH eyes, PREMULTIPLIED — the
//       other blend state, and a world-locked non-fronto-parallel case.
//   (C) LOCAL space, LEFT EYE ONLY, straight alpha — the eye-visibility
//       check (see the submission site for why it is not VIEW space).
// Sizes and positions are fractions of the CANVAS, not absolute metres: a 3D
// display's Kooima frustum is narrow and strongly off-axis, so metre-scale
// poses tuned for an HMD land off-tile. Values match the Metal probe (#1584)
// so the two backends' atlas dumps are directly comparable.
static bool g_quadTest = false;
static bool g_quadActive = false;
static XrSwapchain g_quadSwapchain = XR_NULL_HANDLE;
static GLuint g_quadGLTexture = 0;
static uint32_t g_quadTexSize = 256;
static const float kQuadFracA = 0.53f;  // (A) edge, as a fraction of canvas height
static const float kQuadFracB = 0.67f;  // (B)
static const float kQuadFracC = 0.375f; // (C)

// (A)'s LOCAL pose, in canvas units. The ZDP is the display plane (z = 0), so
// a quad AT z = 0 has zero stereo disparity and tells you nothing; (A) is
// placed just BEHIND it, at about the depth of the spinning cube, so its
// per-tile offset is directly comparable with the cube's. It also sits over
// the cube, so its half-alpha quadrant has something to show through.
static const float kQuadAx = 0.057f;  // × canvas width
static const float kQuadAy = 0.207f;  // × canvas height
static const float kQuadAz = -0.08f;  // × canvas height (-z = behind the plane)
static long g_frameCounter = 0;
static const long kQuadActivationFrame = 10;

// Performance stats
static double g_avgFrameTime = 0.0;
static float g_hudUpdateTimer = 0.0f;
static uint32_t g_windowW = 1512, g_windowH = 823;
static uint32_t g_renderW = 0, g_renderH = 0;

// Cached HUD section strings, refreshed at HUD throttle rate (~2 Hz).
static std::wstring g_hudSessionText, g_hudModeText, g_hudPerfText;
static std::wstring g_hudDisplayText, g_hudEyeText, g_hudCameraText;
static std::wstring g_hudStereoText, g_hudHelpText;

static void SignalHandler(int)
{
    g_running = false;
}

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

static bool CreateMacOSWindow(uint32_t width, uint32_t height, int32_t screenLeft, int32_t screenTop)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        g_appDelegate = [[AppDelegate alloc] init];
        [NSApp setDelegate:g_appDelegate];

        // INV-1.3: open on the 3D panel (#715). (screenLeft, screenTop) is the
        // panel top-left in top-down global coordinates (origin = primary
        // top-left, XrDisplayDesktopPositionDXR); flip into AppKit's bottom-up
        // space. (0,0) = primary — the titled window is auto-constrained below
        // the menu bar, so it is always a safe create position.
        NSRect frame = NSMakeRect(100, 100, width, height);
        NSScreen *primary = [NSScreen screens].firstObject;
        if (primary != nil) {
            CGFloat topY = primary.frame.size.height - (CGFloat)screenTop;
            frame = NSMakeRect((CGFloat)screenLeft, topY - (CGFloat)height, width, height);
        }
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

        // HUD now lives as an XR_EXT_window_space_layer composed by the runtime.

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
                    // Edit steadyIpdFactor (the ModeSwitch ramp target); seed
                    // ipdFactor in lockstep for the idle/non-ramp render path.
                    float v = g_input.viewParams.steadyIpdFactor * factor;
                    if (v < 0.0f) v = 0.0f;
                    if (v > 1.0f) v = 1.0f;
                    g_input.viewParams.steadyIpdFactor = v;
                    g_input.viewParams.ipdFactor = v;
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

    // XR_DXR_display_info
    bool hasDisplayInfoExt;
    float displayWidthM;
    float displayHeightM;
    float nominalViewerX, nominalViewerY, nominalViewerZ;
    uint32_t displayPixelWidth, displayPixelHeight;
    float recommendedViewScaleX, recommendedViewScaleY;
    // v16 XrDisplayDesktopPositionDXR — 3D panel top-left in virtual-desktop
    // pixels (top-down, origin = primary top-left); (0,0) = primary/unknown.
    int32_t displayScreenLeft, displayScreenTop;
    PFN_xrRequestDisplayModeDXR pfnRequestDisplayModeEXT;
    PFN_xrRequestDisplayRenderingModeDXR pfnRequestDisplayRenderingModeEXT;
    PFN_xrEnumerateDisplayRenderingModesDXR pfnEnumerateDisplayRenderingModesEXT;

    // XR_DXR_atlas_capture (W6 of #396): runtime-owned 'I'-key atlas capture.
    bool hasAtlasCaptureExt = false;
    bool hasViewRigExt = false;  // XR_DXR_view_rig (#396 W7)
    PFN_xrCaptureAtlasDXR pfnCaptureAtlasEXT = nullptr;
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

    bool hasOpenGLEnable = false;
    app.hasCocoaWindowBinding = false;
    app.hasMacosGlBinding = false;
    app.hasDisplayInfoExt = false;

    LOG_INFO("Available OpenXR extensions:");
    for (auto &e : exts) {
        LOG_INFO("  %s v%u", e.extensionName, e.extensionVersion);
        if (strcmp(e.extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0)
            hasOpenGLEnable = true;
        if (strcmp(e.extensionName, XR_DXR_COCOA_WINDOW_BINDING_EXTENSION_NAME) == 0)
            app.hasCocoaWindowBinding = true;
        if (strcmp(e.extensionName, XR_DXR_MACOS_GL_BINDING_EXTENSION_NAME) == 0)
            app.hasMacosGlBinding = true;
        if (strcmp(e.extensionName, XR_DXR_DISPLAY_INFO_EXTENSION_NAME) == 0)
            app.hasDisplayInfoExt = true;
        if (strcmp(e.extensionName, XR_DXR_ATLAS_CAPTURE_EXTENSION_NAME) == 0)
            app.hasAtlasCaptureExt = true;
        if (strcmp(e.extensionName, XR_DXR_VIEW_RIG_EXTENSION_NAME) == 0)
            app.hasViewRigExt = true;
    }

    if (!app.hasMacosGlBinding) {
        LOG_ERROR("Runtime does not support XR_DXR_macos_gl_binding");
        return false;
    }
    if (!app.hasCocoaWindowBinding) {
        LOG_WARN("Runtime does not support XR_DXR_cocoa_window_binding — will create own window");
    }
    LOG_INFO("XR_DXR_display_info: %s", app.hasDisplayInfoExt ? "available" : "not available");
    LOG_INFO("XR_DXR_atlas_capture: %s", app.hasAtlasCaptureExt ? "available" : "not available");
    LOG_INFO("XR_DXR_view_rig: %s", app.hasViewRigExt ? "AVAILABLE" : "NOT FOUND");

    // Enable extensions
    std::vector<const char *> enabledExts = {XR_DXR_MACOS_GL_BINDING_EXTENSION_NAME};
    if (hasOpenGLEnable) {
        enabledExts.push_back(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
    }
    if (app.hasCocoaWindowBinding) {
        enabledExts.push_back(XR_DXR_COCOA_WINDOW_BINDING_EXTENSION_NAME);
    }
    if (app.hasDisplayInfoExt) {
        enabledExts.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);
    }
    if (app.hasAtlasCaptureExt) {
        enabledExts.push_back(XR_DXR_ATLAS_CAPTURE_EXTENSION_NAME);
    }
    if (app.hasViewRigExt) {
        enabledExts.push_back(XR_DXR_VIEW_RIG_EXTENSION_NAME);
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
    LOG_INFO("XR_DXR_macos_gl_binding: enabled");
    LOG_INFO("XR_DXR_cocoa_window_binding: %s", app.hasCocoaWindowBinding ? "enabled" : "not available");

    // Get system
    XrSystemGetInfo sysInfo = {XR_TYPE_SYSTEM_GET_INFO};
    sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(app.instance, &sysInfo, &app.systemId));
    LOG_INFO("Got system ID: %llu", (unsigned long long)app.systemId);

    // Get system name and display info
    {
        XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayInfoDXR displayInfo = {};
        displayInfo.type = XR_TYPE_DISPLAY_INFO_DXR;
        // INV-1.3: panel desktop position, so the window below opens on the
        // 3D panel instead of the primary monitor (spec v16, #715).
        XrDisplayDesktopPositionDXR desktopPos = {};
        desktopPos.type = XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR;
        if (app.hasDisplayInfoExt) {
            sysProps.next = &displayInfo;
            displayInfo.next = &desktopPos;
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
                app.displayScreenLeft = desktopPos.left;
                app.displayScreenTop = desktopPos.top;
                LOG_INFO("Display desktop position: (%d, %d)", app.displayScreenLeft, app.displayScreenTop);
                LOG_INFO("Display pixels: %ux%u", app.displayPixelWidth, app.displayPixelHeight);
                LOG_INFO("Display info: %.3fx%.3f m, scale=%.2fx%.2f, nominal=(%.3f,%.3f,%.3f)",
                    app.displayWidthM, app.displayHeightM,
                    app.recommendedViewScaleX, app.recommendedViewScaleY,
                    app.nominalViewerX, app.nominalViewerY, app.nominalViewerZ);
            }
        }
        if (app.hasDisplayInfoExt) {
            xrGetInstanceProcAddr(app.instance, "xrRequestDisplayModeDXR",
                (PFN_xrVoidFunction*)&app.pfnRequestDisplayModeEXT);
            xrGetInstanceProcAddr(app.instance, "xrRequestDisplayRenderingModeDXR",
                (PFN_xrVoidFunction*)&app.pfnRequestDisplayRenderingModeEXT);
            xrGetInstanceProcAddr(app.instance, "xrEnumerateDisplayRenderingModesDXR",
                (PFN_xrVoidFunction*)&app.pfnEnumerateDisplayRenderingModesEXT);
        }
        if (app.hasAtlasCaptureExt) {
            xrGetInstanceProcAddr(app.instance, "xrCaptureAtlasDXR",
                (PFN_xrVoidFunction*)&app.pfnCaptureAtlasEXT);
            LOG_INFO("xrCaptureAtlasDXR: %s", app.pfnCaptureAtlasEXT ? "resolved" : "NULL");
        }
    }

    // Enumerate view configs
    // #1486: this app derives its per-frame view count from the ACTIVE DXR
    // rendering mode, so it must run on the view configuration that reports the
    // device MAX (4 on sim_display Quad). PRIMARY_STEREO now reports exactly 2
    // and xrEndFrame rejects viewCount > 2 under it. Falls back to
    // PRIMARY_STEREO on a runtime that doesn't enumerate the DXR type.
    app.viewConfigType = DxrSelectViewConfigType(app.instance, app.systemId);
    LOG_INFO("View configuration type: %s", DxrViewConfigTypeName(app.viewConfigType));
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

    XrGraphicsBindingOpenGLMacOSDXR glBinding = {};
    glBinding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_MACOS_DXR;
    glBinding.cglContext = (void *)cglCtx;
    glBinding.cglPixelFormat = nullptr;

    // Chain the cocoa window binding — pass our NSView to the runtime
    XrCocoaWindowBindingCreateInfoDXR cocoaBinding = {};
    cocoaBinding.type = XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_DXR;
    cocoaBinding.next = nullptr;
    cocoaBinding.viewHandle = (__bridge void *)g_glView;

    if (app.hasCocoaWindowBinding) {
        glBinding.next = &cocoaBinding;
        LOG_INFO("Chaining XR_DXR_cocoa_window_binding with NSView %p", cocoaBinding.viewHandle);
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
            std::vector<XrDisplayRenderingModeInfoDXR> modes(modeCount);
            for (uint32_t i = 0; i < modeCount; i++) {
                modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR;
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
// #1581 — quad-layer probe texture
// ============================================================================

// Fill one static 256x256 RGBA image and hand it to a swapchain (acquire /
// fill / release once; the layers reference the released image every frame).
//
// Every feature here exists to make a specific defect visible in the atlas
// dump, so do not "tidy" it into a symmetric pattern:
//   - 8 px opaque WHITE border  -> the quad's exact extent, so its width can
//                                  be MEASURED in tile pixels and compared
//                                  against the fov the app was handed.
//   - four distinct corner blocks (TL red, TR green, BL blue, BR yellow)
//                                  -> catches a transpose or a 180 deg flip
//                                     that a mirror-symmetric pattern hides.
//   - a large "Q" with its tail at the BOTTOM-RIGHT
//                                  -> catches a Y flip (tail moves to the
//                                     top) and a UV transpose (tail moves to
//                                     the left).
//   - 24 px coloured checker      -> filtering / scale sanity.
//   - bottom-left quadrant at alpha 128 (STRAIGHT alpha)
//                                  -> the alpha blend: the cube must show
//                                     through it.
//
// Rows are authored TOP-DOWN (row 0 = the image's top) and uploaded with
// glTexSubImage2D, so uv.y = 0 is the top — the same mapping the Metal probe
// gets from replaceRegion. That is what VS_QUAD's `pos.y = -pos.y` pairs with.
// (A GL app that RENDERS into its swapchain instead gets the opposite
// orientation and signals it with XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB
// -> xrt_layer_data::flip_y, which the compositor honours in post_transform.)
static bool CreateAndFillQuadTexture(AppXrSession &app, uint32_t size)
{
    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = 0x8058; // GL_RGBA8
    sci.sampleCount = 1;
    sci.width = size;
    sci.height = size;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(app.session, &sci, &g_quadSwapchain))) {
        return false;
    }

    uint32_t n = 0;
    xrEnumerateSwapchainImages(g_quadSwapchain, 0, &n, nullptr);
    std::vector<XrSwapchainImageOpenGLKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
    if (n == 0 || XR_FAILED(xrEnumerateSwapchainImages(g_quadSwapchain, n, &n,
                                                       (XrSwapchainImageBaseHeader *)imgs.data()))) {
        return false;
    }

    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t idx = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(g_quadSwapchain, &ai, &idx))) {
        return false;
    }
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(g_quadSwapchain, &wi);

    const size_t stride = (size_t)size * 4;
    uint8_t *buf = (uint8_t *)malloc(stride * size);
    if (buf == nullptr) {
        XrSwapchainImageReleaseInfo rr = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(g_quadSwapchain, &rr);
        return false;
    }

    auto put = [&](uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        uint8_t *px = buf + (size_t)y * stride + (size_t)x * 4;
        px[0] = r; px[1] = g; px[2] = b; px[3] = a;
    };

    const float fs = (float)size;
    const uint32_t border = 8;
    const uint32_t corner = size / 5;

    // Ring + tail geometry for the "Q" (normalized, origin top-left).
    const float cx = 0.47f * fs, cy = 0.46f * fs;
    const float rOut = 0.28f * fs, rIn = 0.175f * fs;
    const float tx0 = 0.56f * fs, ty0 = 0.60f * fs;   // tail start (inside the ring)
    const float tx1 = 0.80f * fs, ty1 = 0.86f * fs;   // tail end (bottom-right)
    const float tailHalf = 0.035f * fs;

    for (uint32_t y = 0; y < size; y++) {
        for (uint32_t x = 0; x < size; x++) {
            // Coloured checker base (same RGB values as the Metal probe).
            bool check = (((x / 24) + (y / 24)) & 1) != 0;
            uint8_t r = check ? 120 : 25;
            uint8_t g = check ? 185 : 75;
            uint8_t b = check ? 190 : 100;

            // Corner blocks, inside the border.
            const bool left = (x >= border && x < border + corner);
            const bool right = (x + border + corner >= size && x + border < size);
            const bool top = (y >= border && y < border + corner);
            const bool bottom = (y + border + corner >= size && y + border < size);
            if (left && top)          { r = 255; g = 0;   b = 0;   }  // TL red
            else if (right && top)    { r = 40;  g = 210; b = 40;  }  // TR green
            else if (left && bottom)  { r = 0;   g = 60;  b = 255; }  // BL blue
            else if (right && bottom) { r = 235; g = 220; b = 20;  }  // BR yellow

            // The "Q": black ring + a tail running to the bottom-right.
            const float dx = (float)x - cx, dy = (float)y - cy;
            const float d = sqrtf(dx * dx + dy * dy);
            bool ink = (d <= rOut && d >= rIn);
            if (!ink) {
                // Distance from the point to the tail segment.
                const float sx = tx1 - tx0, sy = ty1 - ty0;
                const float px = (float)x - tx0, py = (float)y - ty0;
                float t = (px * sx + py * sy) / (sx * sx + sy * sy);
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                const float qx = px - t * sx, qy = py - t * sy;
                ink = sqrtf(qx * qx + qy * qy) <= tailHalf;
            }
            if (ink) { r = 15; g = 15; b = 15; }

            // Bottom-left quadrant at half alpha (STRAIGHT alpha bytes).
            uint8_t a = (x < size / 2 && y >= size / 2) ? 128 : 255;

            // Opaque white border last, so it is never punched by the alpha
            // quadrant — it is the measurement fiducial.
            if (x < border || y < border || x + border >= size || y + border >= size) {
                r = g = b = 255;
                a = 255;
            }

            put(x, y, r, g, b, a);
        }
    }

    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, imgs[idx].image);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)size, (GLsizei)size,
                    GL_RGBA, GL_UNSIGNED_BYTE, buf);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    g_quadGLTexture = imgs[idx].image;
    free(buf);

    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_quadSwapchain, &ri);
    g_quadTexSize = size;
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
        case (XrStructureType)XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_DXR: {
            auto* modeEvent = (XrEventDataRenderingModeChangedDXR*)&event;
            LOG_INFO("Rendering mode changed: %u -> %u",
                modeEvent->previousModeIndex, modeEvent->currentModeIndex);
            app.currentModeIndex = modeEvent->currentModeIndex;
            break;
        }
        case (XrStructureType)XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_DXR: {
            // Edge-triggered tracking loss/recovery (#441 v14); HUD state
            // also refreshes per-frame from the XrViewEyeTrackingStateDXR chain.
            auto* etEvent = (XrEventDataEyeTrackingStateChangedDXR*)&event;
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

    LOG_INFO("=== OpenGL Cube OpenXR (External Window, macOS) ===");

    // #1581 quad layers.
    {
        const char *e = getenv("DXR_TEST_QUAD");
        g_quadTest = (e != NULL && e[0] != '\0' && e[0] != '0');
        if (g_quadTest) {
            // The HUD is a window-space layer drawn AFTER the quads; keep it
            // off so the atlas dump is unambiguous. (It is already off by
            // default in this app — this makes the guarantee explicit.)
            g_input.hudVisible = false;
            LOG_INFO("DXR_TEST_QUAD=1 — submitting 3 XrCompositionLayerQuad layers "
                     "(axis-aligned, yawed, LEFT-eye-only)");
        }
    }

    // Initialize OpenXR FIRST — xrGetSystemProperties needs only instance +
    // system id, and returns the panel desktop position the window below is
    // created at (INV-1.3 ordering: instance → system → properties → window
    // → session).
    AppXrSession app = {};
    if (!InitializeOpenXR(app)) {
        LOG_ERROR("Failed to initialize OpenXR");
        return 1;
    }

    // Create the macOS window with OpenGL context (app-owned) on the 3D panel
    if (!CreateMacOSWindow(1512, 823, app.displayScreenLeft, app.displayScreenTop)) {
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

    // HUD window-space layer swapchain (XR_EXT_window_space_layer).
    // GL on macOS routes through the Metal native compositor with IOSurface-
    // backed textures, so the HUD swapchain images may be GL_TEXTURE_RECTANGLE
    // instead of GL_TEXTURE_2D. Detect once at init time (same trick the main
    // swapchain uses).
    XrHudSwapchain hudSwapchain;
    HudRendererMacOS hudRenderer = {};
    std::vector<GLuint> hudGLTextures;
    GLenum hudTexTarget = GL_TEXTURE_2D;
    bool hudReady = false;
    if (CreateHudSwapchain(app.session, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT, hudSwapchain)) {
        std::vector<XrSwapchainImageOpenGLKHR> hudGL(hudSwapchain.imageCount,
            {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
        if (XR_SUCCEEDED(xrEnumerateSwapchainImages(hudSwapchain.swapchain,
                hudSwapchain.imageCount, &hudSwapchain.imageCount,
                (XrSwapchainImageBaseHeader*)hudGL.data()))) {
            hudGLTextures.resize(hudSwapchain.imageCount);
            for (uint32_t i = 0; i < hudSwapchain.imageCount; i++) {
                hudGLTextures[i] = hudGL[i].image;
            }
            // Detect target: bind as 2D and ask for width — 0 means it's
            // actually a rectangle texture (matches the pattern used for
            // the main swapchain at line ~605).
            if (!hudGLTextures.empty()) {
                GLint prev = 0;
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
                glBindTexture(GL_TEXTURE_2D, hudGLTextures[0]);
                GLint w = 0;
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
                glBindTexture(GL_TEXTURE_2D, prev);
                hudTexTarget = (w > 0) ? GL_TEXTURE_2D : GL_TEXTURE_RECTANGLE;
            }
            if (InitializeHudRenderer(hudRenderer, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT)) {
                hudReady = true;
                LOG_INFO("HUD window-space layer ready (%ux%u, %u images, format %lld, target %s)",
                    HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT, hudSwapchain.imageCount,
                    (long long)hudSwapchain.format,
                    hudTexTarget == GL_TEXTURE_2D ? "GL_TEXTURE_2D" : "GL_TEXTURE_RECTANGLE");
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
    // (set during xrEnumerateDisplayRenderingModesDXR above). Fallback is
    // mode 1 (default of app.currentModeIndex).

    g_input.viewParams.virtualDisplayHeight = 0.24f;
    g_input.initialVirtualDisplayHeight = g_input.viewParams.virtualDisplayHeight;
    g_input.nominalViewerZ = app.nominalViewerZ;

    LOG_INFO("Entering main loop... (ESC to quit, drag to rotate, WASD to move, Space to reset)");
    LOG_INFO("Controls: WASD/QE=Move, Drag=Look, Scroll=Scale, Space=Reset, V=Mode, Shift+Tab=HUD, ESC=Quit");

    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !app.exitRequested) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        PumpMacOSEvents();
        PollEvents(app);
        g_frameCounter++;

        // #1581: create + fill the quad probe texture once the session runs.
        if (g_quadTest && !g_quadActive && g_frameCounter >= kQuadActivationFrame) {
            static bool quadAttempted = false;
            if (!quadAttempted) {
                quadAttempted = true;
                g_quadActive = CreateAndFillQuadTexture(app, 256);
                LOG_INFO("Quad probe texture %s (256x256, GL tex %u)",
                         g_quadActive ? "ready" : "FAILED", g_quadGLTexture);
            }
        }

        // Rendering mode requests (V=cycle next, 0-8=jump absolute) through the
        // dxr::ModeSwitch sequencer: it eases g_input.viewParams.ipdFactor around
        // the switch and fires xrRequestDisplayRenderingModeDXR on the right frame.
        // The runtime owns the current mode; the XrEventDataRenderingModeChangedDXR
        // event updates app.currentModeIndex (render paths + HUD read it directly).
        if (!g_modeSwitchConfigured) {
            g_modeSwitch.configure(0.18f, dxr::ModeSwitchEasing::SmoothStep);
            g_modeSwitchConfigured = true;
        }
        {
            const float steady = g_input.viewParams.steadyIpdFactor;
            auto vcOf = [&](uint32_t m) -> uint32_t {
                return (m < app.renderingModeCount && app.renderingModeViewCounts[m] > 0)
                           ? app.renderingModeViewCounts[m] : 1;
            };
            int32_t target = -1;
            if (g_input.cycleRenderingModeRequested) {
                g_input.cycleRenderingModeRequested = false;
                if (app.renderingModeCount > 0)
                    target = (int32_t)((app.currentModeIndex + 1) % app.renderingModeCount);
            }
            if (g_input.absoluteRenderingModeRequested >= 0) {
                int32_t a = g_input.absoluteRenderingModeRequested;
                g_input.absoluteRenderingModeRequested = -1;
                if ((uint32_t)a < app.renderingModeCount) target = a;
            }
            if (target >= 0 && app.session != XR_NULL_HANDLE && app.pfnRequestDisplayRenderingModeEXT) {
                const float curIpd = g_modeSwitch.active() ? g_modeSwitch.ipd() : steady;
                g_modeSwitch.request((uint32_t)target, vcOf((uint32_t)target),
                                     app.currentModeIndex, vcOf(app.currentModeIndex),
                                     curIpd, steady);
            }
            if (g_modeSwitch.active()) {
                float ipd = steady; bool fire = false; uint32_t mode = app.currentModeIndex;
                g_modeSwitch.update(dt, &ipd, &fire, &mode);
                g_input.viewParams.ipdFactor = ipd;
                if (fire && mode != app.currentModeIndex && app.session != XR_NULL_HANDLE &&
                    app.pfnRequestDisplayRenderingModeEXT)
                    app.pfnRequestDisplayRenderingModeEXT(app.session, mode);
            } else {
                g_input.viewParams.ipdFactor = steady;
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

        // XR_DXR_view_rig (#396 W7): drive the runtime rig matching the app's
        // current mode (C selects the rig) with the app's tunables — the
        // runtime owns the window resolve and the Kooima math, and returns
        // render-ready XrView{pose, fov}. Per-locate semantics: chain the rig
        // on every consume locate. The raw result struct feeds the HUD's
        // display-space eye readout (under the rig, views[] carries world
        // eyes, not raw display eyes).
        const bool useRig =
            app.hasViewRigExt && app.displayWidthM > 0 && app.displayHeightM > 0;
        const bool rigCamera = useRig && g_input.cameraMode;
        XrCameraRigDXR cameraRig = {XR_TYPE_CAMERA_RIG_DXR};
        XrDisplayRigDXR displayRig = {XR_TYPE_DISPLAY_RIG_DXR};
        XrViewDisplayRawDXR viewRigRaw = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
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

        // ADR-041: the projection layer must carry EVERY view xrLocateViews
        // returned, not just the ones the active mode renders. Render eyeCount
        // tiles, submit locatedCount views, alias the inactive tail below.
        uint32_t locatedCount = (viewCount > 0) ? viewCount : (uint32_t)eyeCount;
        if (eyeCount > (int)locatedCount) eyeCount = (int)locatedCount;

        // Dynamic arrays for N-view rendering
        std::vector<XrCompositionLayerProjectionView> projViews(locatedCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        // Render
        if (frameState.shouldRender && viewCount >= 1) {
            // Save display-space eye positions for the HUD. Under the rig,
            // views[] carries render-ready world eyes — the raw channel
            // (XrViewDisplayRawDXR) keeps the HUD readout in display space.
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
            // pose). mat4_from_xr_fov is GL [-1,1]-convention — no remap.
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

            // ADR-041: fill the inactive tail [eyeCount, locatedCount). Each
            // keeps its OWN located pose/fov; only the subimage is aliased onto
            // view 0's, and the runtime discards those pixels.
            DxrAliasInactiveViews(projViews.data(), views.data(), locatedCount, (uint32_t)eyeCount);

            RenderScene(renderer, app.swapchain.images[imageIndex],
                        app.swapchain.width, app.swapchain.height,
                        eyeParams.data(), eyeCount);

            // 'I' key: snapshot the multi-view atlas via the runtime-owned
            // XR_DXR_atlas_capture (W6 of #396) — the runtime does the readback
            // from its own atlas image, so the app keeps no staging texture.
            // PROJECTION_ONLY = the app's own projection atlas, pre-compose.
            // Skipped for mono (1×1) layouts; the runtime appends
            // "_atlas_<viewCount>_<cols>x<rows>.png".
            if (g_input.captureAtlasRequested) {
                g_input.captureAtlasRequested = false;
                if (app.pfnCaptureAtlasEXT && app.session != XR_NULL_HANDLE) {
                    if (display3D && (tileColumns > 1 || tileRows > 1)) {
                        std::string prefix = dxr_capture::MakeCaptureAtlasPrefix(
                            "cube_handle_gl_macos", tileColumns, tileRows);
                        XrAtlasCaptureInfoDXR info = {XR_TYPE_ATLAS_CAPTURE_INFO_DXR};
                        info.next = nullptr;
                        info.stage = XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_DXR;
                        strncpy(info.pathPrefix, prefix.c_str(),
                                XR_ATLAS_CAPTURE_PATH_MAX_DXR - 1);
                        info.pathPrefix[XR_ATLAS_CAPTURE_PATH_MAX_DXR - 1] = '\0';
                        XrResult cr = app.pfnCaptureAtlasEXT(app.session, &info, nullptr);
                        if (XR_SUCCEEDED(cr)) {
                            LOG_INFO("Atlas capture requested -> %s_atlas_%u_%ux%u.png",
                                     prefix.c_str(), tileColumns * tileRows, tileColumns, tileRows);
                            dxr_capture::TriggerCaptureFlash((__bridge void*)g_glView);
                        } else {
                            LOG_WARN("xrCaptureAtlasDXR failed: 0x%x", (unsigned)cr);
                        }
                    } else {
                        LOG_WARN("Capture skipped: need 3D mode with cols/rows > 1");
                    }
                } else {
                    LOG_WARN("Atlas capture unavailable: XR_DXR_atlas_capture not active");
                }
            }
        }

        // Release swapchain image
        XrSwapchainImageReleaseInfo relInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(app.swapchain.swapchain, &relInfo);

        // Render HUD into the window-space layer swapchain (when visible).
        bool hudSubmitted = false;
        if (hudReady && g_input.hudVisible && rendered && frameState.shouldRender) {
            uint32_t hudIndex = 0;
            if (AcquireHudSwapchainImage(hudSwapchain, hudIndex)) {
                uint32_t rowPitch = 0;
                const void* pixels = RenderHudAndMap(hudRenderer, &rowPitch,
                    g_hudSessionText, g_hudModeText, g_hudPerfText, g_hudDisplayText,
                    g_hudEyeText, g_hudCameraText, g_hudStereoText, g_hudHelpText);
                if (pixels) {
                    GLint prev = 0;
                    if (hudTexTarget == GL_TEXTURE_2D) {
                        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
                    } else {
                        glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE, &prev);
                    }
                    glBindTexture(hudTexTarget, hudGLTextures[hudIndex]);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                    glTexSubImage2D(hudTexTarget, 0, 0, 0,
                        (GLsizei)HUD_PIXEL_WIDTH, (GLsizei)HUD_PIXEL_HEIGHT,
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                    glBindTexture(hudTexTarget, prev);
                    UnmapHud(hudRenderer);
                    hudSubmitted = true;
                }
                ReleaseHudSwapchainImage(hudSwapchain);
            }
        }

        // #1581 — publish what the quad pass OUGHT to produce, so the atlas
        // dump can be checked against arithmetic instead of against a feeling.
        //
        // Quad (A) is axis-aligned with the eye and lives in LOCAL — the same
        // space the projection layer's view poses are submitted in — so its
        // projected width is exactly
        //     px = size_m * tile_w_px / ((tan(fovR) - tan(fovL)) * d)
        // where d is the eye-to-quad distance along the eye's -Z. Everything
        // on the right-hand side is read from what the runtime just handed us
        // at xrLocateViews; nothing is assumed about the compositor's camera.
        // A compositor using the old hardcoded +-45 deg camera misses this by
        // 1.7-2.7x, which is the whole point of the check.
        if (g_quadTest && g_quadActive && viewCount >= 1 && g_renderW > 0) {
            static int quadExpectLogged = 0;
            if (g_frameCounter > 60 && quadExpectLogged < 2) {
                quadExpectLogged++;
                const float cw = (g_input.canvasWidthM > 0.01f) ? g_input.canvasWidthM : 0.44f;
                const float ch = (g_input.canvasHeightM > 0.01f) ? g_input.canvasHeightM : 0.24f;
                const float sizeA = kQuadFracA * ch;
                const XrVector3f qa = {kQuadAx * cw, kQuadAy * ch, kQuadAz * ch};

                LOG_INFO("[QUAD-EXPECT] canvas=%.4fx%.4f m  quadA size=%.4f m at LOCAL "
                         "(%.4f,%.4f,%.4f)",
                         cw, ch, sizeA, qa.x, qa.y, qa.z);

                for (uint32_t v = 0; v < viewCount && v < 8; v++) {
                    const XrPosef &e = views[v].pose;
                    XrQuaternionf inv = {-e.orientation.x, -e.orientation.y, -e.orientation.z,
                                         e.orientation.w};
                    float lx, ly, lz;
                    quat_rotate_vec3(inv, qa.x - e.position.x, qa.y - e.position.y,
                                     qa.z - e.position.z, &lx, &ly, &lz);
                    const float d = -lz;
                    const float tanL = tanf(views[v].fov.angleLeft);
                    const float tanR = tanf(views[v].fov.angleRight);
                    const float tanSpanX = tanR - tanL;
                    const float expectPx = (d > 0.001f && tanSpanX > 0.0001f)
                                               ? (sizeA * (float)g_renderW / (tanSpanX * d))
                                               : 0.0f;
                    // Tile-local x and y of the quad CENTRE, same derivation.
                    // y matters as much as x: the vertical mapping is where a
                    // Vulkan-vs-GL clip-Y convention slip hides, and an
                    // asymmetric Kooima fov turns that slip into a large
                    // displacement rather than a no-op.
                    const float tanU = tanf(views[v].fov.angleUp);
                    const float tanD = tanf(views[v].fov.angleDown);
                    const float tanSpanY = tanU - tanD;
                    const float centrePx =
                        (d > 0.001f && tanSpanX > 0.0001f)
                            ? ((lx / d) - tanL) / tanSpanX * (float)g_renderW
                            : 0.0f;
                    const float centrePy =
                        (d > 0.001f && tanSpanY > 0.0001f)
                            ? (tanU - (ly / d)) / tanSpanY * (float)g_renderH
                            : 0.0f;
                    LOG_INFO("[QUAD-EXPECT] view=%u tile=%ux%u fovLRUD=(%.4f,%.4f,%.4f,%.4f) rad "
                             "eye=(%.4f,%.4f,%.4f) quadA_eyerel=(%.4f,%.4f,%.4f) d=%.4f m "
                             "tanSpanX=%.4f -> width %.1f px (%.1f%% of tile), centre (%.1f, %.1f) px",
                             v, g_renderW, g_renderH, views[v].fov.angleLeft,
                             views[v].fov.angleRight, views[v].fov.angleUp,
                             views[v].fov.angleDown, e.position.x, e.position.y, e.position.z,
                             lx, ly, lz, d, tanSpanX, expectPx,
                             100.0f * expectPx / (float)g_renderW, centrePx, centrePy);
                }
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
                projViews.data(), locatedCount,
                hudSwapchain, 0.0f, 0.0f, fracW, fracH, 0.0f);
        } else {
            XrCompositionLayerProjection projLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projLayer.space = app.localSpace;
            projLayer.viewCount = locatedCount;
            projLayer.views = projViews.data();

            const XrCompositionLayerBaseHeader *layers[8] = {
                (XrCompositionLayerBaseHeader *)&projLayer
            };
            uint32_t layerCount = (rendered && frameState.shouldRender) ? 1 : 0;

            // #1581 — three quad layers, all on the one probe texture.
            XrCompositionLayerQuad quadA = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            XrCompositionLayerQuad quadB = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            XrCompositionLayerQuad quadC = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            if (layerCount > 0 && g_quadActive && g_quadSwapchain != XR_NULL_HANDLE) {
                XrSwapchainSubImage sub = {};
                sub.swapchain = g_quadSwapchain;
                sub.imageRect.offset = {0, 0};
                sub.imageRect.extent = {(int32_t)g_quadTexSize, (int32_t)g_quadTexSize};
                sub.imageArrayIndex = 0;

                // Everything is sized and placed in units of the CANVAS, not
                // in absolute metres: a 3D display's Kooima frustum is narrow
                // and strongly off-axis vertically, so absolute-metre poses
                // that read fine on an HMD land off-tile here. Fractions of
                // the canvas keep all three quads inside both tiles on any
                // display, which is what makes the picture judgeable.
                const float cw = (g_input.canvasWidthM > 0.01f) ? g_input.canvasWidthM : 0.44f;
                const float ch = (g_input.canvasHeightM > 0.01f) ? g_input.canvasHeightM : 0.24f;

                // (A) LOCAL space, axis-aligned, both eyes, STRAIGHT alpha.
                // This is the measurement quad: LOCAL is the same space the
                // projection layer's view poses are submitted in, so the
                // expected projected width is exactly computable from
                // xrLocateViews (see [QUAD-EXPECT] above). Also the alpha and
                // stereo-disparity probe.
                quadA.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                quadA.space = app.localSpace;
                quadA.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quadA.subImage = sub;
                quadA.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                quadA.pose.position = {kQuadAx * cw, kQuadAy * ch, kQuadAz * ch};
                quadA.size = {kQuadFracA * ch, kQuadFracA * ch};
                layers[layerCount++] = (XrCompositionLayerBaseHeader *)&quadA;

                // (B) LOCAL space, yawed -15 deg about up, both eyes, and
                // deliberately WITHOUT the source-alpha bit so the
                // premultiplied blend state is exercised too (its half-alpha
                // quadrant reads brighter than (A)'s — expected).
                const float halfYaw = -15.0f * 0.5f * (float)M_PI / 180.0f;
                quadB.layerFlags = 0;
                quadB.space = app.localSpace;
                quadB.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quadB.subImage = sub;
                quadB.pose.orientation = {0.0f, sinf(halfYaw), 0.0f, cosf(halfYaw)};
                quadB.pose.position = {0.45f * cw, -0.29f * ch, -0.5f * ch};
                quadB.size = {kQuadFracB * ch, kQuadFracB * ch};
                layers[layerCount++] = (XrCompositionLayerBaseHeader *)&quadB;

                // (C) LEFT EYE ONLY — its absence from the right-hand tile is
                // the eye-visibility check.
                //
                // LOCAL, not VIEW, and that is a finding rather than a
                // preference: a VIEW-space layer pose comes out of
                // oxr_session_frame_end's handle_space HEAD-RELATIVE, but
                // every other layer pose — and the projection view poses the
                // camera is built from — is resolved into the head xdev's
                // TRACKING frame, in which the head sits ~1.6 m up. So a
                // VIEW-space quad lands ~1.6 m below everything else and falls
                // straight out of the frustum. This is a pre-existing
                // cross-backend space bug (#1584 confirmed it on Metal, and
                // D3D11 has it too), not a #1581 regression, and it belongs
                // with #1580's camera/space work. Using VIEW here would test
                // that bug instead of the quad pass.
                quadC.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                quadC.space = app.localSpace;
                quadC.eyeVisibility = XR_EYE_VISIBILITY_LEFT;
                quadC.subImage = sub;
                quadC.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                quadC.pose.position = {-0.379f * cw, -0.311f * ch, 0.0f};
                quadC.size = {kQuadFracC * ch, kQuadFracC * ch};
                layers[layerCount++] = (XrCompositionLayerBaseHeader *)&quadC;
            }

            XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = frameState.predictedDisplayTime;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            endInfo.layerCount = layerCount;
            endInfo.layers = layers;

            xrEndFrame(app.session, &endInfo);
        }

        // FPS tracking
        g_avgFrameTime = g_avgFrameTime * 0.95 + dt * 0.05;

        // Refresh cached HUD section strings (consumed each frame by RenderHudAndMap).
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
            snprintf(buf, sizeof(buf), "%s (OpenGL)\nSession: %s",
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
                "XR_DXR_cocoa_window_binding: %s (OpenGL)\nMode: %s (%s)%s\nKooima: %s",
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
