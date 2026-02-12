// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenGL rendering implementation for cube and grid
 */

#include "gl_renderer.h"
#include "logging.h"
#include <cmath>
#include <vector>

using namespace DirectX;

// GLSL vertex shader for cube
static const char* g_cubeVertSrc = R"(
#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;
uniform mat4 uTransform;
out vec4 vColor;
void main() {
    gl_Position = uTransform * vec4(aPosition, 1.0);
    vColor = aColor;
}
)";

static const char* g_cubeFragSrc = R"(
#version 330 core
in vec4 vColor;
out vec4 FragColor;
void main() {
    FragColor = vColor;
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

struct CubeVertex { float pos[3]; float color[4]; };
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

    renderer.gridProgram = CreateProgram(g_gridVertSrc, g_gridFragSrc);
    if (!renderer.gridProgram) return false;

    // Cube geometry (same vertex data as D3D11 version)
    CubeVertex cubeVerts[] = {
        // Front (red)
        {{-0.5f,-0.5f,-0.5f},{1,0,0,1}}, {{-0.5f,0.5f,-0.5f},{1,0,0,1}},
        {{0.5f,0.5f,-0.5f},{1,0,0,1}},   {{0.5f,-0.5f,-0.5f},{1,0,0,1}},
        // Back (green)
        {{-0.5f,-0.5f,0.5f},{0,1,0,1}},  {{0.5f,-0.5f,0.5f},{0,1,0,1}},
        {{0.5f,0.5f,0.5f},{0,1,0,1}},    {{-0.5f,0.5f,0.5f},{0,1,0,1}},
        // Top (blue)
        {{-0.5f,0.5f,-0.5f},{0,0,1,1}},  {{-0.5f,0.5f,0.5f},{0,0,1,1}},
        {{0.5f,0.5f,0.5f},{0,0,1,1}},    {{0.5f,0.5f,-0.5f},{0,0,1,1}},
        // Bottom (yellow)
        {{-0.5f,-0.5f,-0.5f},{1,1,0,1}}, {{0.5f,-0.5f,-0.5f},{1,1,0,1}},
        {{0.5f,-0.5f,0.5f},{1,1,0,1}},   {{-0.5f,-0.5f,0.5f},{1,1,0,1}},
        // Left (magenta)
        {{-0.5f,-0.5f,0.5f},{1,0,1,1}},  {{-0.5f,0.5f,0.5f},{1,0,1,1}},
        {{-0.5f,0.5f,-0.5f},{1,0,1,1}},  {{-0.5f,-0.5f,-0.5f},{1,0,1,1}},
        // Right (cyan)
        {{0.5f,-0.5f,-0.5f},{0,1,1,1}},  {{0.5f,0.5f,-0.5f},{0,1,1,1}},
        {{0.5f,0.5f,0.5f},{0,1,1,1}},    {{0.5f,-0.5f,0.5f},{0,1,1,1}},
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
    glVertexAttribPointer_(0, 3, GL_FLOAT, GL_FALSE, sizeof(CubeVertex), (void*)0);
    glEnableVertexAttribArray_(1);
    glVertexAttribPointer_(1, 4, GL_FLOAT, GL_FALSE, sizeof(CubeVertex), (void*)(3 * sizeof(float)));

    glBindVertexArray_(0);

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

bool CreateSwapchainFBOs(GLRenderer& renderer, int eye,
    const GLuint* images, uint32_t count,
    uint32_t width, uint32_t height)
{
    // Create depth renderbuffer for this eye
    glGenRenderbuffers_(1, &renderer.depthRBOs[eye]);
    glBindRenderbuffer_(GL_RENDERBUFFER, renderer.depthRBOs[eye]);
    glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);

    // Create one FBO per swapchain image
    renderer.fbos[eye].resize(count);
    glGenFramebuffers_(count, renderer.fbos[eye].data());

    for (uint32_t i = 0; i < count; i++) {
        glBindFramebuffer_(GL_FRAMEBUFFER, renderer.fbos[eye][i]);
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[i], 0);
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, renderer.depthRBOs[eye]);

        if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR("FBO incomplete for eye %d image %u", eye, i);
            return false;
        }
    }

    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    LOG_INFO("Created %u FBOs for eye %d (%ux%u)", count, eye, width, height);
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
    int eye, uint32_t imageIndex,
    uint32_t width, uint32_t height,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix,
    float zoomScale
) {
    glBindFramebuffer_(GL_FRAMEBUFFER, renderer.fbos[eye][imageIndex]);

    // TODO: If the swapchain format is GL_SRGB8_ALPHA8, we may need
    // glEnable(GL_FRAMEBUFFER_SRGB) here for correct linear-to-sRGB conversion.
    // Without it, output may appear too dark compared to the D3D11 version.

    glViewport(0, 0, width, height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    glClearColor(0.05f, 0.05f, 0.25f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Zoom in eye space: scales about the viewer's position, keeping the viewport
    // center (= display center in Kooima projection) fixed on screen.
    XMMATRIX zoom = XMMatrixScaling(zoomScale, zoomScale, zoomScale);

    // Draw cube - base rests on grid at y=0
    {
        const float cubeSize = 0.06f;
        const float cubeHeight = cubeSize / 2.0f;  // Raise by half size so base is at y=0
        XMMATRIX cubeScale = XMMatrixScaling(cubeSize, cubeSize, cubeSize);
        XMMATRIX cubeRot = XMMatrixRotationY(renderer.cubeRotation);
        XMMATRIX cubeTrans = XMMatrixTranslation(0.0f, cubeHeight, 0.0f);
        XMMATRIX cubeWVP = cubeRot * cubeScale * cubeTrans * viewMatrix * zoom * projMatrix;

        glUseProgram_(renderer.cubeProgram);
        SetMatrix(renderer.cubeProgram, "uTransform", cubeWVP);

        glBindVertexArray_(renderer.cubeVAO);
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);
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
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
}

void CleanupGLRenderer(GLRenderer& renderer) {
    for (int eye = 0; eye < 2; eye++) {
        if (!renderer.fbos[eye].empty()) {
            glDeleteFramebuffers_((GLsizei)renderer.fbos[eye].size(), renderer.fbos[eye].data());
            renderer.fbos[eye].clear();
        }
        if (renderer.depthRBOs[eye]) {
            glDeleteRenderbuffers_(1, &renderer.depthRBOs[eye]);
            renderer.depthRBOs[eye] = 0;
        }
    }

    if (renderer.gridVBO) { glDeleteBuffers_(1, &renderer.gridVBO); renderer.gridVBO = 0; }
    if (renderer.gridVAO) { glDeleteVertexArrays_(1, &renderer.gridVAO); renderer.gridVAO = 0; }
    if (renderer.cubeEBO) { glDeleteBuffers_(1, &renderer.cubeEBO); renderer.cubeEBO = 0; }
    if (renderer.cubeVBO) { glDeleteBuffers_(1, &renderer.cubeVBO); renderer.cubeVBO = 0; }
    if (renderer.cubeVAO) { glDeleteVertexArrays_(1, &renderer.cubeVAO); renderer.cubeVAO = 0; }

    if (renderer.gridProgram) { glDeleteProgram_(renderer.gridProgram); renderer.gridProgram = 0; }
    if (renderer.cubeProgram) { glDeleteProgram_(renderer.cubeProgram); renderer.cubeProgram = 0; }
}
