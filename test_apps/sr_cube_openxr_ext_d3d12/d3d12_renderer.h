// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D12 rendering for cube and grid
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <vector>

using Microsoft::WRL::ComPtr;

struct D3D12Renderer {
    // Device and queues
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<IDXGIFactory4> dxgiFactory;

    // Command recording
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;

    // Pipeline
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> cubePSO;
    ComPtr<ID3D12PipelineState> gridPSO;

    // Resources
    ComPtr<ID3D12Resource> cubeVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW cubeVBV = {};
    ComPtr<ID3D12Resource> cubeIndexBuffer;
    D3D12_INDEX_BUFFER_VIEW cubeIBV = {};
    ComPtr<ID3D12Resource> gridVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW gridVBV = {};
    int gridVertexCount = 0;

    // Descriptor heaps
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    UINT rtvDescriptorSize = 0;
    UINT rtvCount = 0;

    // Depth buffers (one per eye)
    ComPtr<ID3D12Resource> depthBuffers[2];
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    UINT dsvDescriptorSize = 0;

    // Synchronization
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = nullptr;

    // Scene state
    float cubeRotation = 0.0f;

    // Swapchain format (typed, e.g. DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
    DXGI_FORMAT swapchainFormat = DXGI_FORMAT_UNKNOWN;
};

struct D3D12ConstantBuffer {
    DirectX::XMFLOAT4X4 worldViewProj;
    DirectX::XMFLOAT4 color;
};

// Initialize D3D12 device on a specific adapter
bool InitializeD3D12WithLUID(D3D12Renderer& renderer, LUID adapterLuid);

// Create render target views for swapchain images.
// format: the typed DXGI format for RTVs (resources may be typeless).
bool CreateSwapchainRTVs(D3D12Renderer& renderer,
    ID3D12Resource** textures, uint32_t count, int eye,
    uint32_t width, uint32_t height,
    DXGI_FORMAT format);

// Update scene state
void UpdateScene(D3D12Renderer& renderer, float deltaTime);

// Render the scene to a swapchain image
void RenderScene(
    D3D12Renderer& renderer,
    ID3D12Resource* renderTarget,
    int rtvIndex,
    int eye,
    uint32_t width, uint32_t height,
    const DirectX::XMMATRIX& viewMatrix,
    const DirectX::XMMATRIX& projMatrix,
    float zoomScale = 1.0f
);

// Wait for GPU to finish
void WaitForGpu(D3D12Renderer& renderer);

// Clean up
void CleanupD3D12(D3D12Renderer& renderer);
