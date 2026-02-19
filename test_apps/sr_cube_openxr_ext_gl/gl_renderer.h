// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenGL rendering for cube and grid
 */

#pragma once

#include "gl_functions.h"
#include <DirectXMath.h>
#include <vector>

struct GLRenderer {
    // Shader programs
    GLuint cubeProgram = 0;
    GLuint gridProgram = 0;

    // Cube geometry
    GLuint cubeVAO = 0;
    GLuint cubeVBO = 0;
    GLuint cubeEBO = 0;

    // Grid geometry
    GLuint gridVAO = 0;
    GLuint gridVBO = 0;
    int gridVertexCount = 0;

    // Single set of FBOs and depth renderbuffer (SBS swapchain)
    // Indexed as: fbos[imageIndex]
    std::vector<GLuint> fbos;
    GLuint depthRBO = 0;

    // Scene state
    float cubeRotation = 0.0f;
};

// Initialize OpenGL renderer (create shaders, geometry)
bool InitializeGLRenderer(GLRenderer& renderer);

// Create FBOs for swapchain images (single SBS swapchain)
bool CreateSwapchainFBOs(GLRenderer& renderer,
    const GLuint* images, uint32_t count,
    uint32_t width, uint32_t height);

// Update scene
void UpdateScene(GLRenderer& renderer, float deltaTime);

// Render to a specific FBO with viewport offset (SBS layout)
void RenderScene(
    GLRenderer& renderer,
    uint32_t imageIndex,
    uint32_t viewportX, uint32_t viewportY,
    uint32_t width, uint32_t height,
    const DirectX::XMMATRIX& viewMatrix,
    const DirectX::XMMATRIX& projMatrix,
    float zoomScale = 1.0f
);

// Cleanup
void CleanupGLRenderer(GLRenderer& renderer);
