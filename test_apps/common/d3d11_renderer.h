// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 rendering for cube, grid, and scene
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <string>

using Microsoft::WRL::ComPtr;

struct D3D11Renderer {
    // Device and context
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIFactory2> dxgiFactory;

    // Pipeline state
    ComPtr<ID3D11VertexShader> cubeVertexShader;
    ComPtr<ID3D11PixelShader> cubePixelShader;
    ComPtr<ID3D11InputLayout> cubeInputLayout;
    ComPtr<ID3D11Buffer> cubeVertexBuffer;
    ComPtr<ID3D11Buffer> cubeIndexBuffer;
    ComPtr<ID3D11Buffer> constantBuffer;

    // Grid rendering
    ComPtr<ID3D11VertexShader> gridVertexShader;
    ComPtr<ID3D11PixelShader> gridPixelShader;
    ComPtr<ID3D11InputLayout> gridInputLayout;
    ComPtr<ID3D11Buffer> gridVertexBuffer;
    int gridVertexCount = 0;

    // Depth stencil
    ComPtr<ID3D11DepthStencilState> depthStencilState;
    ComPtr<ID3D11RasterizerState> rasterizerState;

    // Scene state
    float cubeRotation = 0.0f;

    // Window size (for aspect ratio)
    uint32_t windowWidth = 1280;
    uint32_t windowHeight = 720;
};

// Constant buffer structure for shaders
struct ConstantBufferData {
    DirectX::XMFLOAT4X4 worldViewProj;
    DirectX::XMFLOAT4 color;
};

// Initialize D3D11 device and create resources (uses default adapter)
bool InitializeD3D11(D3D11Renderer& renderer);

// Initialize D3D11 device on a specific adapter (by LUID)
// This is required for OpenXR - must create device on the GPU that OpenXR specifies
bool InitializeD3D11WithLUID(D3D11Renderer& renderer, LUID adapterLuid);

// Create rendering resources (shaders, buffers)
bool CreateResources(D3D11Renderer& renderer);

// Clean up D3D11 resources
void CleanupD3D11(D3D11Renderer& renderer);

// Update scene state (cube rotation, etc.)
void UpdateScene(D3D11Renderer& renderer, float deltaTime);

// Render the scene to a render target view
// viewMatrix and projMatrix come from OpenXR views (already includes player locomotion
// via reference space offset — see UpdateLocalSpace in xr_session)
// cubeHeight: Y position of cube in world space (1.6 for Monado window, 0 for extension apps)
void RenderScene(
    D3D11Renderer& renderer,
    ID3D11RenderTargetView* rtv,
    ID3D11DepthStencilView* dsv,
    uint32_t width,
    uint32_t height,
    const DirectX::XMMATRIX& viewMatrix,
    const DirectX::XMMATRIX& projMatrix,
    float zoomScale = 1.0f,
    float cubeHeight = 1.6f
);

// Create a render target view for an OpenXR swapchain image
bool CreateRenderTargetView(
    D3D11Renderer& renderer,
    ID3D11Texture2D* texture,
    ID3D11RenderTargetView** rtv
);

// Create a depth stencil view
bool CreateDepthStencilView(
    D3D11Renderer& renderer,
    uint32_t width,
    uint32_t height,
    ID3D11Texture2D** depthTexture,
    ID3D11DepthStencilView** dsv
);

// Render cube with pre-computed MVP matrix (for SR native app)
// mvpData points to 16 floats in column-major order (like mat4f::m)
void RenderCubeWithMVP(
    D3D11Renderer& renderer,
    ID3D11RenderTargetView* rtv,
    ID3D11DepthStencilView* dsv,
    const float* mvpData
);
