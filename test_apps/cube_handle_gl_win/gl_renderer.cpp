// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenGL rendering implementation for cube and grid
 */

#include "gl_renderer.h"
#include "logging.h"
#include <cmath>
#include <cstddef>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <string>

using namespace DirectX;

// GLSL vertex shader for cube
static const char* g_cubeVertSrc = R"(
#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aNormal;
layout(location = 4) in vec3 aTangent;
uniform mat4 uTransform;
uniform mat4 uModel;
out vec2 vUV;
out vec3 vWorldNormal;
out vec3 vWorldTangent;
void main() {
    gl_Position = uTransform * vec4(aPosition, 1.0);
    vUV = aUV;
    mat3 normalMat = mat3(uModel);
    vWorldNormal = normalMat * aNormal;
    vWorldTangent = normalMat * aTangent;
}
)";

static const char* g_cubeFragSrc = R"(
#version 330 core
uniform sampler2D basecolorTex;
uniform sampler2D normalTex;
uniform sampler2D aoTex;
uniform int useTextures;
in vec2 vUV;
in vec3 vWorldNormal;
in vec3 vWorldTangent;
out vec4 FragColor;
void main() {
    if (useTextures == 0) {
        FragColor = vec4(1.0);
        return;
    }
    vec3 basecolor = texture(basecolorTex, vUV).rgb;
    vec3 normalMap = texture(normalTex, vUV).rgb;
    float ao = texture(aoTex, vUV).r;
    vec3 N = normalize(vWorldNormal);
    vec3 T = normalize(vWorldTangent);
    T = normalize(T - dot(T, N) * N);
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);
    vec3 mappedNormal = normalize(TBN * (normalMap * 2.0 - 1.0));
    vec3 lightDir = normalize(vec3(0.3, 0.8, 0.5));
    float diffuse = max(dot(mappedNormal, lightDir), 0.0) * 0.7 + 0.3;
    FragColor = vec4(basecolor * ao * diffuse, 1.0);
}
)";

// GLSL vertex shader for grid
static const char* g_gridVertSrc = R"(
#version 330 core
layout(location = 0) in vec3 aPosition;
uniform mat4 uTransform;
void main() {
    gl_Position = uTransform * vec4(aPosition, 1.0);
}
)";

static const char* g_gridFragSrc = R"(
#version 330 core
uniform vec4 uColor;
out vec4 FragColor;
void main() {
    FragColor = uColor;
}
)";

struct CubeVertex { float pos[3]; float color[4]; float uv[2]; float normal[3]; float tangent[3]; };
struct GridVertex { float pos[3]; };

static GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader_(type);
    glShaderSource_(shader, 1, &source, nullptr);
    glCompileShader_(shader);

    GLint status;
    glGetShaderiv_(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog_(shader, 512, nullptr, log);
        LOG_ERROR("Shader compile error: %s", log);
        return 0;
    }
    return shader;
}

static GLuint CreateProgram(const char* vertSrc, const char* fragSrc) {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, vertSrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragSrc);
    if (!vs || !fs) return 0;

    GLuint program = glCreateProgram_();
    glAttachShader_(program, vs);
    glAttachShader_(program, fs);
    glLinkProgram_(program);

    GLint status;
    glGetProgramiv_(program, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512];
        glGetProgramInfoLog_(program, 512, nullptr, log);
        LOG_ERROR("Program link error: %s", log);
        return 0;
    }

    glDeleteShader_(vs);
    glDeleteShader_(fs);
    return program;
}

bool InitializeGLRenderer(GLRenderer& renderer) {
    // Create shader programs
    renderer.cubeProgram = CreateProgram(g_cubeVertSrc, g_cubeFragSrc);
    if (!renderer.cubeProgram) return false;

    // Set texture sampler uniforms
    glUseProgram_(renderer.cubeProgram);
    GLint basecolorLoc = glGetUniformLocation_(renderer.cubeProgram, "basecolorTex");
    GLint normalLoc = glGetUniformLocation_(renderer.cubeProgram, "normalTex");
    GLint aoLoc = glGetUniformLocation_(renderer.cubeProgram, "aoTex");
    if (basecolorLoc >= 0) glUniform1i_(basecolorLoc, 0);
    if (normalLoc >= 0) glUniform1i_(normalLoc, 1);
    if (aoLoc >= 0) glUniform1i_(aoLoc, 2);
    glUseProgram_(0);

    renderer.gridProgram = CreateProgram(g_gridVertSrc, g_gridFragSrc);
    if (!renderer.gridProgram) return false;

    // Cube geometry with UVs, normals, tangents
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

    unsigned short cubeIndices[] = {
        0,1,2, 0,2,3,  4,5,6, 4,6,7,  8,9,10, 8,10,11,
        12,13,14, 12,14,15,  16,17,18, 16,18,19,  20,21,22, 20,22,23,
    };

    glGenVertexArrays_(1, &renderer.cubeVAO);
    glBindVertexArray_(renderer.cubeVAO);

    glGenBuffers_(1, &renderer.cubeVBO);
    glBindBuffer_(GL_ARRAY_BUFFER, renderer.cubeVBO);
    glBufferData_(GL_ARRAY_BUFFER, sizeof(cubeVerts), cubeVerts, GL_STATIC_DRAW);

    glGenBuffers_(1, &renderer.cubeEBO);
    glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, renderer.cubeEBO);
    glBufferData_(GL_ELEMENT_ARRAY_BUFFER, sizeof(cubeIndices), cubeIndices, GL_STATIC_DRAW);

    glEnableVertexAttribArray_(0);
    glVertexAttribPointer_(0, 3, GL_FLOAT, GL_FALSE, sizeof(CubeVertex), (void*)offsetof(CubeVertex, pos));
    glEnableVertexAttribArray_(1);
    glVertexAttribPointer_(1, 4, GL_FLOAT, GL_FALSE, sizeof(CubeVertex), (void*)offsetof(CubeVertex, color));
    glEnableVertexAttribArray_(2);
    glVertexAttribPointer_(2, 2, GL_FLOAT, GL_FALSE, sizeof(CubeVertex), (void*)offsetof(CubeVertex, uv));
    glEnableVertexAttribArray_(3);
    glVertexAttribPointer_(3, 3, GL_FLOAT, GL_FALSE, sizeof(CubeVertex), (void*)offsetof(CubeVertex, normal));
    glEnableVertexAttribArray_(4);
    glVertexAttribPointer_(4, 3, GL_FLOAT, GL_FALSE, sizeof(CubeVertex), (void*)offsetof(CubeVertex, tangent));

    glBindVertexArray_(0);

    // Load textures
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string exeDir(exePath);
        exeDir = exeDir.substr(0, exeDir.find_last_of("\\/") + 1);
        std::string texDir = exeDir + "textures/";

        const char* texFiles[3] = {
            "Wood_Crate_001_basecolor.jpg",
            "Wood_Crate_001_normal.jpg",
            "Wood_Crate_001_ambientOcclusion.jpg",
        };

        // Fallback data: white for basecolor/AO, flat blue for normal
        unsigned char whitePixel[4] = {255, 255, 255, 255};
        unsigned char normalPixel[4] = {128, 128, 255, 255};

        glGenTextures(3, renderer.textures);

        for (int i = 0; i < 3; i++) {
            glBindTexture(GL_TEXTURE_2D, renderer.textures[i]);

            std::string path = texDir + texFiles[i];
            int w, h, channels;
            unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);

            if (pixels) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                glGenerateMipmap_(GL_TEXTURE_2D);
                stbi_image_free(pixels);
                LOG_INFO("Loaded texture: %s (%dx%d)", texFiles[i], w, h);
            } else {
                // Fallback 1x1 texture
                unsigned char* fallback = (i == 1) ? normalPixel : whitePixel;
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, fallback);
                LOG_INFO("Using fallback texture for %s", texFiles[i]);
            }

            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        }

        glBindTexture(GL_TEXTURE_2D, 0);
        renderer.texturesLoaded = true;
    }

    // Grid geometry
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

    glGenVertexArrays_(1, &renderer.gridVAO);
    glBindVertexArray_(renderer.gridVAO);

    glGenBuffers_(1, &renderer.gridVBO);
    glBindBuffer_(GL_ARRAY_BUFFER, renderer.gridVBO);
    glBufferData_(GL_ARRAY_BUFFER, gridVerts.size() * sizeof(GridVertex), gridVerts.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray_(0);
    glVertexAttribPointer_(0, 3, GL_FLOAT, GL_FALSE, sizeof(GridVertex), (void*)0);

    glBindVertexArray_(0);

    LOG_INFO("GL renderer initialized");
    return true;
}

bool CreateSwapchainFBOs(GLRenderer& renderer,
    const GLuint* images, uint32_t count,
    uint32_t width, uint32_t height)
{
    // Create depth renderbuffer (single SBS swapchain)
    glGenRenderbuffers_(1, &renderer.depthRBO);
    glBindRenderbuffer_(GL_RENDERBUFFER, renderer.depthRBO);
    glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);

    // Create one FBO per swapchain image
    renderer.fbos.resize(count);
    glGenFramebuffers_(count, renderer.fbos.data());

    for (uint32_t i = 0; i < count; i++) {
        glBindFramebuffer_(GL_FRAMEBUFFER, renderer.fbos[i]);
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[i], 0);
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, renderer.depthRBO);

        if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR("FBO incomplete for image %u", i);
            return false;
        }
    }

    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    LOG_INFO("Created %u FBOs (%ux%u)", count, width, height);
    return true;
}

void UpdateScene(GLRenderer& renderer, float deltaTime) {
    renderer.cubeRotation += deltaTime * 0.5f;
    if (renderer.cubeRotation > XM_2PI) {
        renderer.cubeRotation -= XM_2PI;
    }
}

static void SetMatrix(GLuint program, const char* name, const XMMATRIX& m) {
    GLint loc = glGetUniformLocation_(program, name);
    if (loc >= 0) {
        XMFLOAT4X4 mat;
        XMStoreFloat4x4(&mat, m);
        // GL_FALSE: row-major DirectXMath data read as column-major by GLSL = implicit
        // transpose. Shader's uTransform*v then computes M^T*v, matching HLSL cbuffer
        // behavior where column-major default also auto-transposes row-major XMMATRIX.
        glUniformMatrix4fv_(loc, 1, GL_FALSE, &mat._11);
    }
}

void RenderScene(
    GLRenderer& renderer,
    uint32_t imageIndex,
    uint32_t viewportX, uint32_t viewportY,
    uint32_t width, uint32_t height,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix,
    float zoomScale,
    float cubeY,
    float cubeZ,
    float cubeSize
) {
    glBindFramebuffer_(GL_FRAMEBUFFER, renderer.fbos[imageIndex]);

    // TODO: If the swapchain format is GL_SRGB8_ALPHA8, we may need
    // glEnable(GL_FRAMEBUFFER_SRGB) here for correct linear-to-sRGB conversion.
    // Without it, output may appear too dark compared to the D3D11 version.

    glViewport(viewportX, viewportY, width, height);
    glScissor(viewportX, viewportY, width, height);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    {
        // Transparent-background mode (DISPLAYXR_TRANSPARENT_BG=1): clear to
        // RGBA(0,0,0,0) so the desktop shows through. Runtime's Leia GL DP
        // recovers per-pixel alpha after the weaver and routes through DComp.
        const char *e = getenv("DISPLAYXR_TRANSPARENT_BG");
        bool transparent_bg = e != nullptr && *e != '\0' && *e != '0';
        if (transparent_bg) {
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        } else {
            glClearColor(0.05f, 0.05f, 0.25f, 1.0f);
        }
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Zoom in eye space: scale only x,y (not z) so perspective division doesn't
    // cancel the effect. Keeps the viewport center fixed on screen.
    XMMATRIX zoom = XMMatrixScaling(zoomScale, zoomScale, 1.0f);

    // Draw cube
    {
        XMMATRIX cubeScale = XMMatrixScaling(cubeSize, cubeSize, cubeSize);
        XMMATRIX cubeRot = XMMatrixRotationY(renderer.cubeRotation);
        XMMATRIX cubeTrans = XMMatrixTranslation(0.0f, cubeY, cubeZ);
        XMMATRIX cubeWVP = cubeRot * cubeScale * cubeTrans * viewMatrix * zoom * projMatrix;

        glUseProgram_(renderer.cubeProgram);
        SetMatrix(renderer.cubeProgram, "uTransform", cubeWVP);

        XMMATRIX cubeModel = cubeRot * cubeScale;
        SetMatrix(renderer.cubeProgram, "uModel", cubeModel);

        // Bind textures
        if (renderer.texturesLoaded) {
            GLint useLoc = glGetUniformLocation_(renderer.cubeProgram, "useTextures");
            if (useLoc >= 0) glUniform1i_(useLoc, 1);
            for (int i = 0; i < 3; i++) {
                glActiveTexture_(GL_TEXTURE0 + i);
                glBindTexture(GL_TEXTURE_2D, renderer.textures[i]);
            }
        } else {
            GLint useLoc = glGetUniformLocation_(renderer.cubeProgram, "useTextures");
            if (useLoc >= 0) glUniform1i_(useLoc, 0);
        }

        glBindVertexArray_(renderer.cubeVAO);
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);

        // Unbind textures
        for (int i = 0; i < 3; i++) {
            glActiveTexture_(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        glActiveTexture_(GL_TEXTURE0);
    }

    // Draw grid
    {
        const float gridScale = 0.05f;
        XMMATRIX gridWorld = XMMatrixScaling(gridScale, gridScale, gridScale) *
                             XMMatrixTranslation(0, gridScale, 0);
        XMMATRIX gridWVP = gridWorld * viewMatrix * zoom * projMatrix;

        glUseProgram_(renderer.gridProgram);
        SetMatrix(renderer.gridProgram, "uTransform", gridWVP);
        GLint colorLoc = glGetUniformLocation_(renderer.gridProgram, "uColor");
        if (colorLoc >= 0) glUniform4f_(colorLoc, 0.3f, 0.3f, 0.35f, 1.0f);

        glBindVertexArray_(renderer.gridVAO);
        glDrawArrays(GL_LINES, 0, renderer.gridVertexCount);
    }

    glBindVertexArray_(0);
    glUseProgram_(0);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
}

void CleanupGLRenderer(GLRenderer& renderer) {
    if (renderer.textures[0]) {
        glDeleteTextures(3, renderer.textures);
        renderer.textures[0] = renderer.textures[1] = renderer.textures[2] = 0;
    }

    if (!renderer.fbos.empty()) {
        glDeleteFramebuffers_((GLsizei)renderer.fbos.size(), renderer.fbos.data());
        renderer.fbos.clear();
    }
    if (renderer.depthRBO) {
        glDeleteRenderbuffers_(1, &renderer.depthRBO);
        renderer.depthRBO = 0;
    }

    if (renderer.gridVBO) { glDeleteBuffers_(1, &renderer.gridVBO); renderer.gridVBO = 0; }
    if (renderer.gridVAO) { glDeleteVertexArrays_(1, &renderer.gridVAO); renderer.gridVAO = 0; }
    if (renderer.cubeEBO) { glDeleteBuffers_(1, &renderer.cubeEBO); renderer.cubeEBO = 0; }
    if (renderer.cubeVBO) { glDeleteBuffers_(1, &renderer.cubeVBO); renderer.cubeVBO = 0; }
    if (renderer.cubeVAO) { glDeleteVertexArrays_(1, &renderer.cubeVAO); renderer.cubeVAO = 0; }

    if (renderer.gridProgram) { glDeleteProgram_(renderer.gridProgram); renderer.gridProgram = 0; }
    if (renderer.cubeProgram) { glDeleteProgram_(renderer.cubeProgram); renderer.cubeProgram = 0; }
}
