// Copyright 2025, The DisplayXR Project
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

    // Textures (basecolor, normal, AO)
    GLuint textures[3] = {0, 0, 0};
    bool texturesLoaded = false;

    // FBOs + depth renderbuffer for the projection swapchain.
    // TILED: one FBO per image (fbos[imageIndex]).
    // ARRAY: one FBO per (image, slice) — fbos[imageIndex*arraySize + slice] —
    // each attaching one GL_TEXTURE_2D_ARRAY layer via glFramebufferTextureLayer.
    std::vector<GLuint> fbos;
    GLuint depthRBO = 0;
    bool arrayLayout = false;
    uint32_t arraySize = 1;

    // Scene state
    float cubeRotation = 0.0f;
};

// Initialize OpenGL renderer (create shaders, geometry)
bool InitializeGLRenderer(GLRenderer& renderer);

// Create FBOs for the projection swapchain. TILED (arraySize<=1): one FBO per
// image over a GL_TEXTURE_2D. ARRAY (arraySize>1): one FBO per (image, slice)
// attaching a GL_TEXTURE_2D_ARRAY layer via glFramebufferTextureLayer; `width`
// is the per-view width (one slice), not the atlas width.
bool CreateSwapchainFBOs(GLRenderer& renderer,
    const GLuint* images, uint32_t count,
    uint32_t width, uint32_t height,
    uint32_t arraySize = 1);

// Update scene. spinSpeed (rad/s) is agent-settable via the set_spin
// MCP tool (#457).
void UpdateScene(GLRenderer& renderer, float deltaTime, float spinSpeed = 0.5f);

// Render to a specific FBO with viewport offset (SBS layout). In ARRAY layout,
// `slice` selects the array-layer FBO (viewportX/Y should be 0).
void RenderScene(
    GLRenderer& renderer,
    uint32_t imageIndex,
    uint32_t viewportX, uint32_t viewportY,
    uint32_t width, uint32_t height,
    const DirectX::XMMATRIX& viewMatrix,
    const DirectX::XMMATRIX& projMatrix,
    float zoomScale = 1.0f,
    float cubeY = 0.03f,     // World Y position of cube center (sits on floor)
    float cubeZ = 0.0f,      // World Z position (0 = origin, negative = in front of camera)
    float cubeSize = 0.06f,  // Cube edge length in meters
    uint32_t slice = 0       // ARRAY layout: array layer to render into
);

// Cleanup
void CleanupGLRenderer(GLRenderer& renderer);
