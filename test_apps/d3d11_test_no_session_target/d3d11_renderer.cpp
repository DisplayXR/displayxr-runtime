// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 rendering implementation
 */

#include "d3d11_renderer.h"
#include "logging.h"
#include <d3dcompiler.h>
#include <cmath>
#include <vector>

using namespace DirectX;

// Vertex structure for cube
struct CubeVertex {
    XMFLOAT3 position;
    XMFLOAT3 normal;
};

// Vertex structure for grid lines
struct GridVertex {
    XMFLOAT3 position;
};

// HLSL shader source for cube
static const char* g_cubeShaderSource = R"(
cbuffer Constants : register(b0) {
    float4x4 worldViewProj;
    float4 color;
};

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct PSInput {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    output.position = mul(float4(input.position, 1.0), worldViewProj);
    output.normal = input.normal;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    // Simple directional lighting
    float3 lightDir = normalize(float3(0.5, 1.0, 0.3));
    float ndotl = max(dot(input.normal, lightDir), 0.0);
    float3 ambient = float3(0.2, 0.2, 0.3);
    float3 diffuse = color.rgb * ndotl;
    return float4(ambient + diffuse, 1.0);
}
)";

// HLSL shader source for grid
static const char* g_gridShaderSource = R"(
cbuffer Constants : register(b0) {
    float4x4 worldViewProj;
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
    output.position = mul(float4(input.position, 1.0), worldViewProj);
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return color;
}
)";

static bool CompileShader(const char* source, const char* entryPoint, const char* target, ID3DBlob** blob) {
    LOG_DEBUG("Compiling shader: %s (%s)", entryPoint, target);
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(
        source, strlen(source),
        nullptr, nullptr, nullptr,
        entryPoint, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        blob, &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            LOG_ERROR("Shader compilation failed: %s", (char*)errorBlob->GetBufferPointer());
        } else {
            LogHResult("Shader compilation", hr);
        }
        return false;
    }
    LOG_DEBUG("Shader compiled successfully: %s", entryPoint);
    return true;
}

bool InitializeD3D11(D3D11Renderer& renderer) {
    LOG_INFO("Initializing D3D11...");

    // Create DXGI factory
    LOG_INFO("Creating DXGI factory...");
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&renderer.dxgiFactory));
    if (FAILED(hr)) {
        LogHResult("CreateDXGIFactory1", hr);
        return false;
    }
    LOG_INFO("DXGI factory created: 0x%p", renderer.dxgiFactory.Get());

    // Create D3D11 device
    LOG_INFO("Creating D3D11 device...");
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
    LOG_INFO("Debug layer enabled");
#endif

    hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &renderer.device,
        nullptr,
        &renderer.context
    );

    if (FAILED(hr)) {
        LOG_WARN("D3D11 device creation failed (might be debug layer), retrying without debug...");
        // Try without debug layer
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &renderer.device,
            nullptr,
            &renderer.context
        );
        if (FAILED(hr)) {
            LogHResult("D3D11CreateDevice", hr);
            return false;
        }
    }
    LOG_INFO("D3D11 device created: 0x%p", renderer.device.Get());
    LOG_INFO("D3D11 context created: 0x%p", renderer.context.Get());

    LOG_INFO("Creating D3D11 resources...");
    bool result = CreateResources(renderer);
    if (result) {
        LOG_INFO("D3D11 initialization complete");
    } else {
        LOG_ERROR("Failed to create D3D11 resources");
    }
    return result;
}

bool CreateResources(D3D11Renderer& renderer) {
    HRESULT hr;

    // Compile cube shaders
    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (!CompileShader(g_cubeShaderSource, "VSMain", "vs_5_0", &vsBlob)) {
        return false;
    }
    if (!CompileShader(g_cubeShaderSource, "PSMain", "ps_5_0", &psBlob)) {
        return false;
    }

    hr = renderer.device->CreateVertexShader(
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        nullptr, &renderer.cubeVertexShader);
    if (FAILED(hr)) return false;

    hr = renderer.device->CreatePixelShader(
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
        nullptr, &renderer.cubePixelShader);
    if (FAILED(hr)) return false;

    // Create cube input layout
    D3D11_INPUT_ELEMENT_DESC cubeInputElements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = renderer.device->CreateInputLayout(
        cubeInputElements, ARRAYSIZE(cubeInputElements),
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        &renderer.cubeInputLayout);
    if (FAILED(hr)) return false;

    // Compile grid shaders
    if (!CompileShader(g_gridShaderSource, "VSMain", "vs_5_0", &vsBlob)) {
        return false;
    }
    if (!CompileShader(g_gridShaderSource, "PSMain", "ps_5_0", &psBlob)) {
        return false;
    }

    hr = renderer.device->CreateVertexShader(
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        nullptr, &renderer.gridVertexShader);
    if (FAILED(hr)) return false;

    hr = renderer.device->CreatePixelShader(
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
        nullptr, &renderer.gridPixelShader);
    if (FAILED(hr)) return false;

    // Create grid input layout
    D3D11_INPUT_ELEMENT_DESC gridInputElements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = renderer.device->CreateInputLayout(
        gridInputElements, ARRAYSIZE(gridInputElements),
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        &renderer.gridInputLayout);
    if (FAILED(hr)) return false;

    // Create cube vertex data (with normals for lighting)
    CubeVertex cubeVertices[] = {
        // Front face
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(0, 0, -1) },
        { XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT3(0, 0, -1) },
        { XMFLOAT3( 0.5f,  0.5f, -0.5f), XMFLOAT3(0, 0, -1) },
        { XMFLOAT3( 0.5f, -0.5f, -0.5f), XMFLOAT3(0, 0, -1) },
        // Back face
        { XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT3(0, 0, 1) },
        { XMFLOAT3( 0.5f, -0.5f,  0.5f), XMFLOAT3(0, 0, 1) },
        { XMFLOAT3( 0.5f,  0.5f,  0.5f), XMFLOAT3(0, 0, 1) },
        { XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT3(0, 0, 1) },
        // Top face
        { XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT3(0, 1, 0) },
        { XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT3(0, 1, 0) },
        { XMFLOAT3( 0.5f,  0.5f,  0.5f), XMFLOAT3(0, 1, 0) },
        { XMFLOAT3( 0.5f,  0.5f, -0.5f), XMFLOAT3(0, 1, 0) },
        // Bottom face
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(0, -1, 0) },
        { XMFLOAT3( 0.5f, -0.5f, -0.5f), XMFLOAT3(0, -1, 0) },
        { XMFLOAT3( 0.5f, -0.5f,  0.5f), XMFLOAT3(0, -1, 0) },
        { XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT3(0, -1, 0) },
        // Left face
        { XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT3(-1, 0, 0) },
        { XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT3(-1, 0, 0) },
        { XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT3(-1, 0, 0) },
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(-1, 0, 0) },
        // Right face
        { XMFLOAT3( 0.5f, -0.5f, -0.5f), XMFLOAT3(1, 0, 0) },
        { XMFLOAT3( 0.5f,  0.5f, -0.5f), XMFLOAT3(1, 0, 0) },
        { XMFLOAT3( 0.5f,  0.5f,  0.5f), XMFLOAT3(1, 0, 0) },
        { XMFLOAT3( 0.5f, -0.5f,  0.5f), XMFLOAT3(1, 0, 0) },
    };

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(cubeVertices);
    vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = cubeVertices;

    hr = renderer.device->CreateBuffer(&vbDesc, &vbData, &renderer.cubeVertexBuffer);
    if (FAILED(hr)) return false;

    // Create cube index buffer
    uint16_t cubeIndices[] = {
        0, 1, 2, 0, 2, 3,       // Front
        4, 5, 6, 4, 6, 7,       // Back
        8, 9, 10, 8, 10, 11,    // Top
        12, 13, 14, 12, 14, 15, // Bottom
        16, 17, 18, 16, 18, 19, // Left
        20, 21, 22, 20, 22, 23, // Right
    };

    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = sizeof(cubeIndices);
    ibDesc.Usage = D3D11_USAGE_IMMUTABLE;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA ibData = {};
    ibData.pSysMem = cubeIndices;

    hr = renderer.device->CreateBuffer(&ibDesc, &ibData, &renderer.cubeIndexBuffer);
    if (FAILED(hr)) return false;

    // Create grid vertices (floor at y = -1)
    const int gridSize = 10;
    const float gridSpacing = 1.0f;
    std::vector<GridVertex> gridVertices;

    for (int i = -gridSize; i <= gridSize; i++) {
        // Lines along Z axis
        gridVertices.push_back({ XMFLOAT3((float)i * gridSpacing, -1.0f, -gridSize * gridSpacing) });
        gridVertices.push_back({ XMFLOAT3((float)i * gridSpacing, -1.0f, gridSize * gridSpacing) });
        // Lines along X axis
        gridVertices.push_back({ XMFLOAT3(-gridSize * gridSpacing, -1.0f, (float)i * gridSpacing) });
        gridVertices.push_back({ XMFLOAT3(gridSize * gridSpacing, -1.0f, (float)i * gridSpacing) });
    }
    renderer.gridVertexCount = (int)gridVertices.size();

    vbDesc.ByteWidth = (UINT)(gridVertices.size() * sizeof(GridVertex));
    vbData.pSysMem = gridVertices.data();

    hr = renderer.device->CreateBuffer(&vbDesc, &vbData, &renderer.gridVertexBuffer);
    if (FAILED(hr)) return false;

    // Create constant buffer
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(ConstantBufferData);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = renderer.device->CreateBuffer(&cbDesc, nullptr, &renderer.constantBuffer);
    if (FAILED(hr)) return false;

    // Create depth stencil state
    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D11_COMPARISON_LESS;

    hr = renderer.device->CreateDepthStencilState(&dsDesc, &renderer.depthStencilState);
    if (FAILED(hr)) return false;

    // Create rasterizer state
    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_BACK;
    rsDesc.FrontCounterClockwise = FALSE;
    rsDesc.DepthClipEnable = TRUE;

    hr = renderer.device->CreateRasterizerState(&rsDesc, &renderer.rasterizerState);
    if (FAILED(hr)) return false;

    return true;
}

void CleanupD3D11(D3D11Renderer& renderer) {
    // ComPtr handles cleanup automatically
    renderer.cubeVertexShader.Reset();
    renderer.cubePixelShader.Reset();
    renderer.cubeInputLayout.Reset();
    renderer.cubeVertexBuffer.Reset();
    renderer.cubeIndexBuffer.Reset();
    renderer.gridVertexShader.Reset();
    renderer.gridPixelShader.Reset();
    renderer.gridInputLayout.Reset();
    renderer.gridVertexBuffer.Reset();
    renderer.constantBuffer.Reset();
    renderer.depthStencilState.Reset();
    renderer.rasterizerState.Reset();
    renderer.context.Reset();
    renderer.device.Reset();
    renderer.dxgiFactory.Reset();
}

void UpdateScene(D3D11Renderer& renderer, float deltaTime) {
    // Rotate cube
    renderer.cubeRotation += deltaTime * 0.5f; // Half rotation per second
    if (renderer.cubeRotation > XM_2PI) {
        renderer.cubeRotation -= XM_2PI;
    }
}

static void UpdateConstantBuffer(D3D11Renderer& renderer, const XMMATRIX& wvp, const XMFLOAT4& color) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = renderer.context->Map(renderer.constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        ConstantBufferData* cb = (ConstantBufferData*)mapped.pData;
        XMStoreFloat4x4(&cb->worldViewProj, XMMatrixTranspose(wvp));
        cb->color = color;
        renderer.context->Unmap(renderer.constantBuffer.Get(), 0);
    }
}

void RenderScene(
    D3D11Renderer& renderer,
    ID3D11RenderTargetView* rtv,
    ID3D11DepthStencilView* dsv,
    uint32_t width,
    uint32_t height,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix,
    float cameraPosX,
    float cameraPosY,
    float cameraPosZ,
    float cameraYaw,
    float cameraPitch
) {
    // Set render targets
    renderer.context->OMSetRenderTargets(1, &rtv, dsv);

    // Set viewport
    D3D11_VIEWPORT viewport = {};
    viewport.Width = (float)width;
    viewport.Height = (float)height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    renderer.context->RSSetViewports(1, &viewport);

    // Clear
    float clearColor[] = { 0.1f, 0.1f, 0.15f, 1.0f };
    renderer.context->ClearRenderTargetView(rtv, clearColor);
    renderer.context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);

    // Set states
    renderer.context->OMSetDepthStencilState(renderer.depthStencilState.Get(), 0);
    renderer.context->RSSetState(renderer.rasterizerState.Get());

    // Build camera view matrix from input state
    // Note: For OpenXR stereo rendering, we use the viewMatrix from OpenXR
    // The cameraPosX/Y/Z and yaw/pitch are used to offset the LOCAL reference space
    XMMATRIX cameraOffset = XMMatrixTranslation(-cameraPosX, -cameraPosY, -cameraPosZ);
    XMMATRIX cameraRotation = XMMatrixRotationRollPitchYaw(cameraPitch, cameraYaw, 0);
    XMMATRIX view = cameraRotation * cameraOffset * viewMatrix;

    // Render cube
    XMMATRIX cubeWorld = XMMatrixRotationY(renderer.cubeRotation);
    XMMATRIX cubeWVP = cubeWorld * view * projMatrix;

    renderer.context->IASetInputLayout(renderer.cubeInputLayout.Get());
    renderer.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = sizeof(CubeVertex);
    UINT offset = 0;
    renderer.context->IASetVertexBuffers(0, 1, renderer.cubeVertexBuffer.GetAddressOf(), &stride, &offset);
    renderer.context->IASetIndexBuffer(renderer.cubeIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    renderer.context->VSSetShader(renderer.cubeVertexShader.Get(), nullptr, 0);
    renderer.context->PSSetShader(renderer.cubePixelShader.Get(), nullptr, 0);
    renderer.context->VSSetConstantBuffers(0, 1, renderer.constantBuffer.GetAddressOf());
    renderer.context->PSSetConstantBuffers(0, 1, renderer.constantBuffer.GetAddressOf());

    UpdateConstantBuffer(renderer, cubeWVP, XMFLOAT4(0.4f, 0.6f, 0.9f, 1.0f)); // Blue-ish cube
    renderer.context->DrawIndexed(36, 0, 0);

    // Render grid
    XMMATRIX gridWVP = view * projMatrix; // Grid has no world transform

    renderer.context->IASetInputLayout(renderer.gridInputLayout.Get());
    renderer.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    stride = sizeof(GridVertex);
    renderer.context->IASetVertexBuffers(0, 1, renderer.gridVertexBuffer.GetAddressOf(), &stride, &offset);
    renderer.context->VSSetShader(renderer.gridVertexShader.Get(), nullptr, 0);
    renderer.context->PSSetShader(renderer.gridPixelShader.Get(), nullptr, 0);

    UpdateConstantBuffer(renderer, gridWVP, XMFLOAT4(0.3f, 0.3f, 0.35f, 1.0f)); // Gray grid
    renderer.context->Draw(renderer.gridVertexCount, 0);
}

bool CreateRenderTargetView(
    D3D11Renderer& renderer,
    ID3D11Texture2D* texture,
    ID3D11RenderTargetView** rtv
) {
    return SUCCEEDED(renderer.device->CreateRenderTargetView(texture, nullptr, rtv));
}

bool CreateDepthStencilView(
    D3D11Renderer& renderer,
    uint32_t width,
    uint32_t height,
    ID3D11Texture2D** depthTexture,
    ID3D11DepthStencilView** dsv
) {
    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    HRESULT hr = renderer.device->CreateTexture2D(&depthDesc, nullptr, depthTexture);
    if (FAILED(hr)) return false;

    hr = renderer.device->CreateDepthStencilView(*depthTexture, nullptr, dsv);
    if (FAILED(hr)) {
        (*depthTexture)->Release();
        *depthTexture = nullptr;
        return false;
    }

    return true;
}
