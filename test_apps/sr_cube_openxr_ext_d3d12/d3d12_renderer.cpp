// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D12 rendering implementation for cube and grid
 */

#include "d3d12_renderer.h"
#include "logging.h"
#include <cmath>

using namespace DirectX;

// Vertex structures
struct CubeVertex {
    XMFLOAT3 position;
    XMFLOAT4 color;
};

struct GridVertex {
    XMFLOAT3 position;
};

// HLSL shader source for cube
static const char* g_cubeShaderSource = R"(
cbuffer Constants : register(b0) {
    float4x4 transform;
    float4 color;
};

struct VSInput {
    float3 position : POSITION;
    float4 color : COLOR;
};

struct PSInput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    output.position = mul(transform, float4(input.position, 1.0));
    output.color = input.color;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return input.color;
}
)";

// HLSL shader source for grid
static const char* g_gridShaderSource = R"(
cbuffer Constants : register(b0) {
    float4x4 transform;
    float4 color;
};

struct VSInput {
    float3 position : POSITION;
};

struct PSInput {
    float4 position : SV_POSITION;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    output.position = mul(transform, float4(input.position, 1.0));
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return color;
}
)";

static bool CompileShader(const char* source, const char* entryPoint, const char* target, ID3DBlob** blob) {
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr,
        entryPoint, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            LOG_ERROR("Shader compilation failed: %s", (char*)errorBlob->GetBufferPointer());
        }
        return false;
    }
    return true;
}

static ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, const void* data, UINT64 size) {
    ComPtr<ID3D12Resource> buffer;
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer));
    if (FAILED(hr)) return nullptr;

    if (data) {
        void* mapped = nullptr;
        D3D12_RANGE readRange = {0, 0};
        buffer->Map(0, &readRange, &mapped);
        memcpy(mapped, data, (size_t)size);
        buffer->Unmap(0, nullptr);
    }
    return buffer;
}

static bool CreateResources(D3D12Renderer& renderer) {
    HRESULT hr;

    // Create root signature with one 32-bit constant buffer (push constants via root constants)
    D3D12_ROOT_PARAMETER rootParam = {};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParam.Constants.ShaderRegister = 0;
    rootParam.Constants.RegisterSpace = 0;
    rootParam.Constants.Num32BitValues = sizeof(D3D12ConstantBuffer) / 4;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 1;
    rootSigDesc.pParameters = &rootParam;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob, errorBlob;
    hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        &sigBlob, &errorBlob);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to serialize root signature");
        return false;
    }

    hr = renderer.device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
        sigBlob->GetBufferSize(), IID_PPV_ARGS(&renderer.rootSignature));
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create root signature");
        return false;
    }

    // Compile shaders
    ComPtr<ID3DBlob> cubeVS, cubePS, gridVS, gridPS;
    if (!CompileShader(g_cubeShaderSource, "VSMain", "vs_5_0", &cubeVS)) return false;
    if (!CompileShader(g_cubeShaderSource, "PSMain", "ps_5_0", &cubePS)) return false;
    if (!CompileShader(g_gridShaderSource, "VSMain", "vs_5_0", &gridVS)) return false;
    if (!CompileShader(g_gridShaderSource, "PSMain", "ps_5_0", &gridPS)) return false;

    // Cube input layout
    D3D12_INPUT_ELEMENT_DESC cubeInputElements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // Grid input layout
    D3D12_INPUT_ELEMENT_DESC gridInputElements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // Cube PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = renderer.rootSignature.Get();
    psoDesc.VS = { cubeVS->GetBufferPointer(), cubeVS->GetBufferSize() };
    psoDesc.PS = { cubePS->GetBufferPointer(), cubePS->GetBufferSize() };
    psoDesc.InputLayout = { cubeInputElements, _countof(cubeInputElements) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; // Will be overridden at render time if needed
    psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    psoDesc.SampleDesc.Count = 1;

    hr = renderer.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&renderer.cubePSO));
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create cube PSO: 0x%08X", hr);
        return false;
    }

    // Grid PSO (line list topology)
    psoDesc.VS = { gridVS->GetBufferPointer(), gridVS->GetBufferSize() };
    psoDesc.PS = { gridPS->GetBufferPointer(), gridPS->GetBufferSize() };
    psoDesc.InputLayout = { gridInputElements, _countof(gridInputElements) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    hr = renderer.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&renderer.gridPSO));
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create grid PSO: 0x%08X", hr);
        return false;
    }

    // Create vertex/index buffers (upload heap for simplicity)
    CubeVertex cubeVertices[] = {
        // Front (red)
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT4(1, 0, 0, 1) },
        { XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT4(1, 0, 0, 1) },
        { XMFLOAT3( 0.5f,  0.5f, -0.5f), XMFLOAT4(1, 0, 0, 1) },
        { XMFLOAT3( 0.5f, -0.5f, -0.5f), XMFLOAT4(1, 0, 0, 1) },
        // Back (green)
        { XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT4(0, 1, 0, 1) },
        { XMFLOAT3( 0.5f, -0.5f,  0.5f), XMFLOAT4(0, 1, 0, 1) },
        { XMFLOAT3( 0.5f,  0.5f,  0.5f), XMFLOAT4(0, 1, 0, 1) },
        { XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT4(0, 1, 0, 1) },
        // Top (blue)
        { XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT4(0, 0, 1, 1) },
        { XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT4(0, 0, 1, 1) },
        { XMFLOAT3( 0.5f,  0.5f,  0.5f), XMFLOAT4(0, 0, 1, 1) },
        { XMFLOAT3( 0.5f,  0.5f, -0.5f), XMFLOAT4(0, 0, 1, 1) },
        // Bottom (yellow)
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT4(1, 1, 0, 1) },
        { XMFLOAT3( 0.5f, -0.5f, -0.5f), XMFLOAT4(1, 1, 0, 1) },
        { XMFLOAT3( 0.5f, -0.5f,  0.5f), XMFLOAT4(1, 1, 0, 1) },
        { XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT4(1, 1, 0, 1) },
        // Left (magenta)
        { XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT4(1, 0, 1, 1) },
        { XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT4(1, 0, 1, 1) },
        { XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT4(1, 0, 1, 1) },
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT4(1, 0, 1, 1) },
        // Right (cyan)
        { XMFLOAT3( 0.5f, -0.5f, -0.5f), XMFLOAT4(0, 1, 1, 1) },
        { XMFLOAT3( 0.5f,  0.5f, -0.5f), XMFLOAT4(0, 1, 1, 1) },
        { XMFLOAT3( 0.5f,  0.5f,  0.5f), XMFLOAT4(0, 1, 1, 1) },
        { XMFLOAT3( 0.5f, -0.5f,  0.5f), XMFLOAT4(0, 1, 1, 1) },
    };

    uint16_t cubeIndices[] = {
        0, 1, 2, 0, 2, 3,
        4, 5, 6, 4, 6, 7,
        8, 9, 10, 8, 10, 11,
        12, 13, 14, 12, 14, 15,
        16, 17, 18, 16, 18, 19,
        20, 21, 22, 20, 22, 23,
    };

    renderer.cubeVertexBuffer = CreateUploadBuffer(renderer.device.Get(), cubeVertices, sizeof(cubeVertices));
    if (!renderer.cubeVertexBuffer) return false;
    renderer.cubeVBV.BufferLocation = renderer.cubeVertexBuffer->GetGPUVirtualAddress();
    renderer.cubeVBV.SizeInBytes = sizeof(cubeVertices);
    renderer.cubeVBV.StrideInBytes = sizeof(CubeVertex);

    renderer.cubeIndexBuffer = CreateUploadBuffer(renderer.device.Get(), cubeIndices, sizeof(cubeIndices));
    if (!renderer.cubeIndexBuffer) return false;
    renderer.cubeIBV.BufferLocation = renderer.cubeIndexBuffer->GetGPUVirtualAddress();
    renderer.cubeIBV.SizeInBytes = sizeof(cubeIndices);
    renderer.cubeIBV.Format = DXGI_FORMAT_R16_UINT;

    // Grid vertices
    const int gridSize = 10;
    const float gridSpacing = 1.0f;
    std::vector<GridVertex> gridVertices;

    for (int i = -gridSize; i <= gridSize; i++) {
        gridVertices.push_back({ XMFLOAT3((float)i * gridSpacing, -1.0f, -gridSize * gridSpacing) });
        gridVertices.push_back({ XMFLOAT3((float)i * gridSpacing, -1.0f, gridSize * gridSpacing) });
        gridVertices.push_back({ XMFLOAT3(-gridSize * gridSpacing, -1.0f, (float)i * gridSpacing) });
        gridVertices.push_back({ XMFLOAT3(gridSize * gridSpacing, -1.0f, (float)i * gridSpacing) });
    }
    renderer.gridVertexCount = (int)gridVertices.size();

    UINT gridSize_ = (UINT)(gridVertices.size() * sizeof(GridVertex));
    renderer.gridVertexBuffer = CreateUploadBuffer(renderer.device.Get(), gridVertices.data(), gridSize_);
    if (!renderer.gridVertexBuffer) return false;
    renderer.gridVBV.BufferLocation = renderer.gridVertexBuffer->GetGPUVirtualAddress();
    renderer.gridVBV.SizeInBytes = gridSize_;
    renderer.gridVBV.StrideInBytes = sizeof(GridVertex);

    // Create fence
    hr = renderer.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&renderer.fence));
    if (FAILED(hr)) return false;
    renderer.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!renderer.fenceEvent) return false;

    return true;
}

bool InitializeD3D12WithLUID(D3D12Renderer& renderer, LUID adapterLuid) {
    LOG_INFO("Initializing D3D12 with adapter LUID: 0x%08X%08X",
             adapterLuid.HighPart, adapterLuid.LowPart);

    // Create DXGI factory
    UINT dxgiFlags = 0;
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        dxgiFlags = DXGI_CREATE_FACTORY_DEBUG;
        LOG_INFO("D3D12 debug layer enabled");
    }
#endif

    HRESULT hr = CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&renderer.dxgiFactory));
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create DXGI factory");
        return false;
    }

    // Find adapter by LUID
    ComPtr<IDXGIAdapter1> adapter;
    hr = renderer.dxgiFactory->EnumAdapterByLuid(adapterLuid, IID_PPV_ARGS(&adapter));
    if (FAILED(hr)) {
        LOG_ERROR("Failed to find adapter with LUID");
        return false;
    }

    DXGI_ADAPTER_DESC1 adapterDesc;
    adapter->GetDesc1(&adapterDesc);
    LOG_INFO("Found adapter: %ls", adapterDesc.Description);

    // Create device
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&renderer.device));
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create D3D12 device: 0x%08X", hr);
        return false;
    }
    LOG_INFO("D3D12 device created: 0x%p", renderer.device.Get());

    // Create command queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    hr = renderer.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&renderer.commandQueue));
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create command queue");
        return false;
    }
    LOG_INFO("D3D12 command queue created");

    // Create command allocator and list
    hr = renderer.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&renderer.commandAllocator));
    if (FAILED(hr)) return false;

    hr = renderer.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        renderer.commandAllocator.Get(), nullptr, IID_PPV_ARGS(&renderer.commandList));
    if (FAILED(hr)) return false;
    renderer.commandList->Close(); // Start closed; Reset before use

    // Create resources
    LOG_INFO("Creating D3D12 resources...");
    if (!CreateResources(renderer)) {
        LOG_ERROR("Failed to create D3D12 resources");
        return false;
    }

    LOG_INFO("D3D12 initialization complete");
    return true;
}

bool CreateSwapchainRTVs(D3D12Renderer& renderer,
    ID3D12Resource** textures, uint32_t count, int eye,
    uint32_t width, uint32_t height,
    DXGI_FORMAT format)
{
    // Store swapchain format and recreate PSOs to match on first call.
    // The underlying D3D12 resources are typeless (for Vulkan interop), so
    // both RTVs and PSOs need an explicit typed format.
    if (eye == 0 && renderer.swapchainFormat != format) {
        renderer.swapchainFormat = format;

        // Recreate PSOs with the actual swapchain format
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = renderer.rootSignature.Get();
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;
        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = format;
        psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        psoDesc.SampleDesc.Count = 1;

        // Recompile shaders for PSO recreation
        ComPtr<ID3DBlob> cubeVS, cubePS, gridVS, gridPS;
        if (!CompileShader(g_cubeShaderSource, "VSMain", "vs_5_0", &cubeVS) ||
            !CompileShader(g_cubeShaderSource, "PSMain", "ps_5_0", &cubePS) ||
            !CompileShader(g_gridShaderSource, "VSMain", "vs_5_0", &gridVS) ||
            !CompileShader(g_gridShaderSource, "PSMain", "ps_5_0", &gridPS)) {
            LOG_ERROR("Failed to recompile shaders for format 0x%X", format);
            return false;
        }

        // Cube input layout
        D3D12_INPUT_ELEMENT_DESC cubeInputElements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        psoDesc.VS = { cubeVS->GetBufferPointer(), cubeVS->GetBufferSize() };
        psoDesc.PS = { cubePS->GetBufferPointer(), cubePS->GetBufferSize() };
        psoDesc.InputLayout = { cubeInputElements, _countof(cubeInputElements) };

        renderer.cubePSO.Reset();
        HRESULT hr = renderer.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&renderer.cubePSO));
        if (FAILED(hr)) {
            LOG_ERROR("Failed to recreate cube PSO with format 0x%X: 0x%08X", format, hr);
            return false;
        }

        // Grid input layout
        D3D12_INPUT_ELEMENT_DESC gridInputElements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        psoDesc.VS = { gridVS->GetBufferPointer(), gridVS->GetBufferSize() };
        psoDesc.PS = { gridPS->GetBufferPointer(), gridPS->GetBufferSize() };
        psoDesc.InputLayout = { gridInputElements, _countof(gridInputElements) };
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

        renderer.gridPSO.Reset();
        hr = renderer.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&renderer.gridPSO));
        if (FAILED(hr)) {
            LOG_ERROR("Failed to recreate grid PSO with format 0x%X: 0x%08X", format, hr);
            return false;
        }
        LOG_INFO("Recreated PSOs with swapchain format 0x%X", format);
    }

    // Create RTV descriptor heap (accumulates for both eyes)
    uint32_t totalRTVs = renderer.rtvCount + count;
    if (!renderer.rtvHeap || totalRTVs > renderer.rtvCount) {
        // Recreate heap if needed
        if (eye == 0) {
            // First eye: create fresh heap for both eyes
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
            heapDesc.NumDescriptors = count * 2; // Both eyes
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            HRESULT hr = renderer.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&renderer.rtvHeap));
            if (FAILED(hr)) return false;
            renderer.rtvDescriptorSize = renderer.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            renderer.rtvCount = 0;
        }
    }

    // Create RTVs for this eye's swapchain images.
    // Explicit format is required because resources are typeless (for Vulkan interop).
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = format;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;
    rtvDesc.Texture2D.PlaneSlice = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = renderer.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += renderer.rtvCount * renderer.rtvDescriptorSize;

    for (uint32_t i = 0; i < count; i++) {
        renderer.device->CreateRenderTargetView(textures[i], &rtvDesc, rtvHandle);
        rtvHandle.ptr += renderer.rtvDescriptorSize;
    }
    renderer.rtvCount += count;

    // Create depth buffer for this eye (if not already created)
    if (!renderer.depthBuffers[eye]) {
        if (!renderer.dsvHeap) {
            D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
            dsvHeapDesc.NumDescriptors = 2; // Both eyes
            dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            HRESULT hr = renderer.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&renderer.dsvHeap));
            if (FAILED(hr)) return false;
            renderer.dsvDescriptorSize = renderer.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        }

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC depthDesc = {};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = width;
        depthDesc.Height = height;
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        clearValue.DepthStencil.Depth = 1.0f;
        clearValue.DepthStencil.Stencil = 0;

        HRESULT hr = renderer.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
            IID_PPV_ARGS(&renderer.depthBuffers[eye]));
        if (FAILED(hr)) return false;

        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = renderer.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        dsvHandle.ptr += eye * renderer.dsvDescriptorSize;
        renderer.device->CreateDepthStencilView(renderer.depthBuffers[eye].Get(), nullptr, dsvHandle);
    }

    LOG_INFO("Created %u RTVs + depth for eye %d (%ux%u)", count, eye, width, height);
    return true;
}

void UpdateScene(D3D12Renderer& renderer, float deltaTime) {
    renderer.cubeRotation += deltaTime * 0.5f;
    if (renderer.cubeRotation > XM_2PI) {
        renderer.cubeRotation -= XM_2PI;
    }
}

void RenderScene(
    D3D12Renderer& renderer,
    ID3D12Resource* renderTarget,
    int rtvIndex,
    int eye,
    uint32_t width, uint32_t height,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix,
    float zoomScale
) {
    HRESULT hr;

    // Reset command allocator and list
    hr = renderer.commandAllocator->Reset();
    if (FAILED(hr)) return;
    hr = renderer.commandList->Reset(renderer.commandAllocator.Get(), nullptr);
    if (FAILED(hr)) return;

    auto cmdList = renderer.commandList.Get();

    // Transition render target to RENDER_TARGET state
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = renderTarget;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    // Get RTV and DSV handles
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = renderer.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += rtvIndex * renderer.rtvDescriptorSize;

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = renderer.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsvHandle.ptr += eye * renderer.dsvDescriptorSize;

    // Set render targets
    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    // Clear
    float clearColor[4] = {0.05f, 0.05f, 0.25f, 1.0f};
    cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 0, 0, nullptr);

    // Set viewport and scissor
    D3D12_VIEWPORT viewport = {};
    viewport.Width = (FLOAT)width;
    viewport.Height = (FLOAT)height;
    viewport.MaxDepth = 1.0f;
    cmdList->RSSetViewports(1, &viewport);

    D3D12_RECT scissor = { 0, 0, (LONG)width, (LONG)height };
    cmdList->RSSetScissorRects(1, &scissor);

    cmdList->SetGraphicsRootSignature(renderer.rootSignature.Get());

    XMMATRIX zoom = XMMatrixScaling(zoomScale, zoomScale, zoomScale);

    // Draw cube - base rests on grid at y=0
    {
        const float cubeSize = 0.06f;
        const float cubeHeight = cubeSize / 2.0f;  // Raise by half size so base is at y=0
        XMMATRIX cubeScale = XMMatrixScaling(cubeSize, cubeSize, cubeSize);
        XMMATRIX cubeRot = XMMatrixRotationY(renderer.cubeRotation);
        XMMATRIX cubeTrans = XMMatrixTranslation(0.0f, cubeHeight, 0.0f);
        XMMATRIX cubeWVP = cubeRot * cubeScale * cubeTrans * zoom * viewMatrix * projMatrix;

        D3D12ConstantBuffer cb;
        XMStoreFloat4x4(&cb.worldViewProj, cubeWVP);
        cb.color = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);

        cmdList->SetPipelineState(renderer.cubePSO.Get());
        cmdList->SetGraphicsRoot32BitConstants(0, sizeof(cb) / 4, &cb, 0);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->IASetVertexBuffers(0, 1, &renderer.cubeVBV);
        cmdList->IASetIndexBuffer(&renderer.cubeIBV);
        cmdList->DrawIndexedInstanced(36, 1, 0, 0, 0);
    }

    // Draw grid
    {
        const float gridScale = 0.05f;
        XMMATRIX gridWorld = XMMatrixScaling(gridScale, gridScale, gridScale) *
                             XMMatrixTranslation(0, gridScale, 0);
        XMMATRIX gridWVP = gridWorld * zoom * viewMatrix * projMatrix;

        D3D12ConstantBuffer cb;
        XMStoreFloat4x4(&cb.worldViewProj, gridWVP);
        cb.color = XMFLOAT4(0.3f, 0.3f, 0.35f, 1.0f);

        cmdList->SetPipelineState(renderer.gridPSO.Get());
        cmdList->SetGraphicsRoot32BitConstants(0, sizeof(cb) / 4, &cb, 0);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        cmdList->IASetVertexBuffers(0, 1, &renderer.gridVBV);
        cmdList->DrawInstanced(renderer.gridVertexCount, 1, 0, 0);
    }

    // Transition render target back
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    cmdList->ResourceBarrier(1, &barrier);

    // Execute
    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList };
    renderer.commandQueue->ExecuteCommandLists(1, lists);

    // Signal and wait
    renderer.fenceValue++;
    renderer.commandQueue->Signal(renderer.fence.Get(), renderer.fenceValue);
    if (renderer.fence->GetCompletedValue() < renderer.fenceValue) {
        renderer.fence->SetEventOnCompletion(renderer.fenceValue, renderer.fenceEvent);
        WaitForSingleObject(renderer.fenceEvent, INFINITE);
    }
}

void WaitForGpu(D3D12Renderer& renderer) {
    if (!renderer.commandQueue || !renderer.fence) return;
    renderer.fenceValue++;
    renderer.commandQueue->Signal(renderer.fence.Get(), renderer.fenceValue);
    if (renderer.fence->GetCompletedValue() < renderer.fenceValue) {
        renderer.fence->SetEventOnCompletion(renderer.fenceValue, renderer.fenceEvent);
        WaitForSingleObject(renderer.fenceEvent, INFINITE);
    }
}

void CleanupD3D12(D3D12Renderer& renderer) {
    WaitForGpu(renderer);

    if (renderer.fenceEvent) {
        CloseHandle(renderer.fenceEvent);
        renderer.fenceEvent = nullptr;
    }

    renderer.fence.Reset();
    renderer.depthBuffers[0].Reset();
    renderer.depthBuffers[1].Reset();
    renderer.dsvHeap.Reset();
    renderer.rtvHeap.Reset();
    renderer.gridVertexBuffer.Reset();
    renderer.cubeIndexBuffer.Reset();
    renderer.cubeVertexBuffer.Reset();
    renderer.gridPSO.Reset();
    renderer.cubePSO.Reset();
    renderer.rootSignature.Reset();
    renderer.commandList.Reset();
    renderer.commandAllocator.Reset();
    renderer.commandQueue.Reset();
    renderer.device.Reset();
    renderer.dxgiFactory.Reset();
}
