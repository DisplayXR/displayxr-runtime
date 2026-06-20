// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenGL rendering for cube and grid + display-zones tile rendering
 *
 * Cloned from cube_handle_gl_win (GL render) + cube_zones_d3d11_win (zones
 * logic) for #613. The cube/grid renderer is unchanged from the handle app;
 * the zone helpers (per-zone FBO/depth, per-tile RenderZoneScene with an
 * UNCONDITIONAL depth clear, and the content-alpha edge-fade GLSL pass) are
 * the GL port of the D3D11 zones app's tile-render path.
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

    // Single set of FBOs and depth renderbuffer (SBS swapchain, fallback path)
    // Indexed as: fbos[imageIndex]
    std::vector<GLuint> fbos;
    GLuint depthRBO = 0;

    // Zone edge-fade pass (content-alpha feather). Lazily created.
    GLuint fadeProgram = 0;
    GLuint fadeVAO = 0;        // empty VAO for the attribute-less fullscreen triangle
    bool fadeInitTried = false;

    // Scene state
    float cubeRotation = 0.0f;
};

// ---------------------------------------------------------------------------
// Display-zones tile resources (one per zone)
//
// Each zone owns its own horizontally tiled swapchain (width = tileW*tileCount)
// and a matching depth renderbuffer, with one FBO per swapchain image. Mirrors
// the per-zone ID3D11DepthStencilView + per-image RTV the D3D11 app builds.
// ---------------------------------------------------------------------------
struct GLZoneResources {
    std::vector<GLuint> fbos;   // one per swapchain image
    GLuint depthRBO = 0;
    uint32_t fullW = 0;         // tileW * tileCount
    uint32_t fullH = 0;
};

// Initialize OpenGL renderer (create shaders, geometry)
bool InitializeGLRenderer(GLRenderer& renderer);

// Create FBOs for swapchain images (single SBS swapchain, fallback path)
bool CreateSwapchainFBOs(GLRenderer& renderer,
    const GLuint* images, uint32_t count,
    uint32_t width, uint32_t height);

// Create per-zone FBOs + depth for a horizontally tiled zone swapchain.
bool CreateZoneFBOs(GLZoneResources& zr,
    const GLuint* images, uint32_t count,
    uint32_t fullWidth, uint32_t fullHeight);

void DestroyZoneFBOs(GLZoneResources& zr);

// Update scene. spinSpeed (rad/s) is agent-settable via the set_spin MCP tool.
void UpdateScene(GLRenderer& renderer, float deltaTime, float spinSpeed = 0.5f);

// Render to a specific FBO with viewport offset (SBS layout, fallback path)
void RenderScene(
    GLRenderer& renderer,
    uint32_t imageIndex,
    uint32_t viewportX, uint32_t viewportY,
    uint32_t width, uint32_t height,
    const DirectX::XMMATRIX& viewMatrix,
    const DirectX::XMMATRIX& projMatrix,
    float zoomScale = 1.0f,
    float cubeY = 0.03f,
    float cubeZ = 0.0f,
    float cubeSize = 0.06f
);

// Render one zone view tile into the zone's FBO.
//
// HARD GOTCHA (#613): depth MUST be cleared every tile, every frame (with
// depth writes enabled), or the cube renders as a dark z-failed shadow (the
// D3D12 zones bug). Color clear is CONDITIONAL on `clearColor != nullptr`
// (the caller pre-clears the whole tiled image once per zone with the zone's
// premultiplied background); depth clear is UNCONDITIONAL.
//
// clearColor: pointer to 4 premultiplied RGBA floats, or nullptr to skip the
//             color clear for this tile (depth still clears).
void RenderZoneTile(
    GLRenderer& renderer,
    GLZoneResources& zr,
    uint32_t imageIndex,
    uint32_t tileX,                 // tile column in the swapchain (px = tileX*tileW)
    uint32_t tileW, uint32_t tileH,
    const DirectX::XMMATRIX& viewMatrix,
    const DirectX::XMMATRIX& projMatrix,
    const float* clearColor,        // nullptr = skip color clear (depth still cleared)
    float cubeY = 0.03f,
    float cubeZ = 0.0f,
    float cubeSize = 0.06f
);

// Content-alpha edge fade (ADR-027 rule 4): multiply the current tile's dst
// alpha down toward the tile edges so the zone blends softly into whatever is
// behind it. GL port of the D3D11 ZERO/INV_SRC_ALPHA blend. featherPx<=0 is a
// no-op. The FBO for `imageIndex` must already be bound by RenderZoneTile or
// re-bound by the caller; this re-binds it and sets the tile viewport itself.
void DrawZoneEdgeFade(
    GLRenderer& renderer,
    GLZoneResources& zr,
    uint32_t imageIndex,
    uint32_t tileX,
    uint32_t tileW, uint32_t tileH,
    float featherPx
);

// Cleanup
void CleanupGLRenderer(GLRenderer& renderer);
