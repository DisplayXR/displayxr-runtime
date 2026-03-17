// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D12 rendering implementation for cube and grid
 */

#include "d3d12_renderer.h"
#include "logging.h"
#include <cmath>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <string>

using namespace DirectX;

// Vertex structures
struct CubeVertex {
    XMFLOAT3 position;
    XMFLOAT4 color;
    XMFLOAT2 uv;
    XMFLOAT3 normal;
    XMFLOAT3 tangent;
};

struct GridVertex {
    XMFLOAT3 position;
};

// HLSL shader source for cube (textured with normal mapping)
static const char* g_cubeShaderSource = R"(
cbuffer Constants : register(b0) {
    float4x4 transform;
    float4 color;
    float4x4 model;
};

Texture2D basecolorTex : register(t0);
Texture2D normalTex : register(t1);
Texture2D aoTex : register(t2);
SamplerState texSampler : register(s0);

struct VSInput {
    float3 position : POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
    float3 worldNormal : NORMAL;
    float3 worldTangent : TANGENT;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    output.position = mul(transform, float4(input.position, 1.0));
    output.uv = input.uv;
    float3x3 normalMat = (float3x3)model;
    output.worldNormal = mul(normalMat, input.normal);
    output.worldTangent = mul(normalMat, input.tangent);
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    float3 basecolor = basecolorTex.Sample(texSampler, input.uv).rgb;
    float3 normalMap = normalTex.Sample(texSampler, input.uv).rgb;
    float ao = aoTex.Sample(texSampler, input.uv).r;

    float3 N = normalize(input.worldNormal);
    float3 T = normalize(input.worldTangent);
    T = normalize(T - dot(T, N) * N);
    float3 B = cross(N, T);
    float3x3 TBN = float3x3(T, B, N);

    float3 mappedNormal = normalize(mul(normalMap * 2.0 - 1.0, TBN));

    float3 lightDir = normalize(float3(0.3, 0.8, 0.5));
    float diffuse = max(dot(mappedNormal, lightDir), 0.0) * 0.7 + 0.3;

    return float4(basecolor * ao * diffuse, 1.0);
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
    // This is used by the grid shader (simple root constants only)
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

    // Cube root signature: root constants + descriptor table for SRVs + static sampler
    {
        D3D12_ROOT_PARAMETER cubeRootParams[2] = {};
        // Param 0: root constants (same as grid but larger for model matrix)
        cubeRootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        cubeRootParams[0].Constants.ShaderRegister = 0;
        cubeRootParams[0].Constants.RegisterSpace = 0;
        cubeRootParams[0].Constants.Num32BitValues = sizeof(D3D12ConstantBuffer) / 4;
        cubeRootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Param 1: descriptor table for 3 SRVs (t0, t1, t2)
        D3D12_DESCRIPTOR_RANGE srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 3;
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = 0;

        cubeRootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        cubeRootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        cubeRootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
        cubeRootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // Static sampler for texture sampling
        D3D12_STATIC_SAMPLER_DESC staticSampler = {};
        staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSampler.ShaderRegister = 0;
        staticSampler.RegisterSpace = 0;
        staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        staticSampler.MaxLOD = D3D12_FLOAT32_MAX;

        D3D12_ROOT_SIGNATURE_DESC cubeRootSigDesc = {};
        cubeRootSigDesc.NumParameters = 2;
        cubeRootSigDesc.pParameters = cubeRootParams;
        cubeRootSigDesc.NumStaticSamplers = 1;
        cubeRootSigDesc.pStaticSamplers = &staticSampler;
        cubeRootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> cubeSigBlob, cubeErrorBlob;
        hr = D3D12SerializeRootSignature(&cubeRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            &cubeSigBlob, &cubeErrorBlob);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to serialize cube root signature");
            return false;
        }
        hr = renderer.device->CreateRootSignature(0, cubeSigBlob->GetBufferPointer(),
            cubeSigBlob->GetBufferSize(), IID_PPV_ARGS(&renderer.cubeRootSignature));
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create cube root signature");
            return false;
        }
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
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // Grid input layout
    D3D12_INPUT_ELEMENT_DESC gridInputElements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // Cube PSO (fallback, non-textured path — still needs cubeRootSignature
    // because the cube shader references SRV/sampler bindings)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = renderer.cubeRootSignature.Get();
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

    // Textured cube PSO (uses cubeRootSignature with SRV descriptor table)
    {
        D3D12_INPUT_ELEMENT_DESC cubeTexInputElements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC cubePsoDesc = {};
        cubePsoDesc.pRootSignature = renderer.cubeRootSignature.Get();
        cubePsoDesc.VS = { cubeVS->GetBufferPointer(), cubeVS->GetBufferSize() };
        cubePsoDesc.PS = { cubePS->GetBufferPointer(), cubePS->GetBufferSize() };
        cubePsoDesc.InputLayout = { cubeTexInputElements, _countof(cubeTexInputElements) };
        cubePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        cubePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        cubePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        cubePsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
        cubePsoDesc.RasterizerState.DepthClipEnable = TRUE;
        cubePsoDesc.DepthStencilState.DepthEnable = TRUE;
        cubePsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        cubePsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        cubePsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        cubePsoDesc.SampleMask = UINT_MAX;
        cubePsoDesc.NumRenderTargets = 1;
        cubePsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        cubePsoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        cubePsoDesc.SampleDesc.Count = 1;

        hr = renderer.device->CreateGraphicsPipelineState(&cubePsoDesc, IID_PPV_ARGS(&renderer.cubePSOTextured));
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create textured cube PSO: 0x%08X", hr);
            // Non-fatal: fall back to non-textured cube
        }
    }

    // Grid PSO (line list topology — uses simple grid root signature)
    psoDesc.pRootSignature = renderer.rootSignature.Get();
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
        // Front face (-Z): normal (0,0,-1), tangent (1,0,0)
        { XMFLOAT3(-0.5f,-0.5f,-0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(0,1), XMFLOAT3(0,0,-1), XMFLOAT3(1,0,0) },
        { XMFLOAT3(-0.5f, 0.5f,-0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(0,0), XMFLOAT3(0,0,-1), XMFLOAT3(1,0,0) },
        { XMFLOAT3( 0.5f, 0.5f,-0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(1,0), XMFLOAT3(0,0,-1), XMFLOAT3(1,0,0) },
        { XMFLOAT3( 0.5f,-0.5f,-0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(1,1), XMFLOAT3(0,0,-1), XMFLOAT3(1,0,0) },
        // Back face (+Z): normal (0,0,1), tangent (-1,0,0)
        { XMFLOAT3(-0.5f,-0.5f, 0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(1,1), XMFLOAT3(0,0,1), XMFLOAT3(-1,0,0) },
        { XMFLOAT3( 0.5f,-0.5f, 0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(0,1), XMFLOAT3(0,0,1), XMFLOAT3(-1,0,0) },
        { XMFLOAT3( 0.5f, 0.5f, 0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(0,0), XMFLOAT3(0,0,1), XMFLOAT3(-1,0,0) },
        { XMFLOAT3(-0.5f, 0.5f, 0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(1,0), XMFLOAT3(0,0,1), XMFLOAT3(-1,0,0) },
        // Top face (+Y): normal (0,1,0), tangent (1,0,0)
        { XMFLOAT3(-0.5f, 0.5f,-0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(0,1), XMFLOAT3(0,1,0), XMFLOAT3(1,0,0) },
        { XMFLOAT3(-0.5f, 0.5f, 0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(0,0), XMFLOAT3(0,1,0), XMFLOAT3(1,0,0) },
        { XMFLOAT3( 0.5f, 0.5f, 0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(1,0), XMFLOAT3(0,1,0), XMFLOAT3(1,0,0) },
        { XMFLOAT3( 0.5f, 0.5f,-0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(1,1), XMFLOAT3(0,1,0), XMFLOAT3(1,0,0) },
        // Bottom face (-Y): normal (0,-1,0), tangent (1,0,0)
        { XMFLOAT3(-0.5f,-0.5f,-0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(0,0), XMFLOAT3(0,-1,0), XMFLOAT3(1,0,0) },
        { XMFLOAT3( 0.5f,-0.5f,-0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(1,0), XMFLOAT3(0,-1,0), XMFLOAT3(1,0,0) },
        { XMFLOAT3( 0.5f,-0.5f, 0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(1,1), XMFLOAT3(0,-1,0), XMFLOAT3(1,0,0) },
        { XMFLOAT3(-0.5f,-0.5f, 0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(0,1), XMFLOAT3(0,-1,0), XMFLOAT3(1,0,0) },
        // Left face (-X): normal (-1,0,0), tangent (0,0,-1)
        { XMFLOAT3(-0.5f,-0.5f, 0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(0,1), XMFLOAT3(-1,0,0), XMFLOAT3(0,0,-1) },
        { XMFLOAT3(-0.5f, 0.5f, 0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(0,0), XMFLOAT3(-1,0,0), XMFLOAT3(0,0,-1) },
        { XMFLOAT3(-0.5f, 0.5f,-0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(1,0), XMFLOAT3(-1,0,0), XMFLOAT3(0,0,-1) },
        { XMFLOAT3(-0.5f,-0.5f,-0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(1,1), XMFLOAT3(-1,0,0), XMFLOAT3(0,0,-1) },
        // Right face (+X): normal (1,0,0), tangent (0,0,1)
        { XMFLOAT3( 0.5f,-0.5f,-0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(0,1), XMFLOAT3(1,0,0), XMFLOAT3(0,0,1) },
        { XMFLOAT3( 0.5f, 0.5f,-0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(0,0), XMFLOAT3(1,0,0), XMFLOAT3(0,0,1) },
        { XMFLOAT3( 0.5f, 0.5f, 0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(1,0), XMFLOAT3(1,0,0), XMFLOAT3(0,0,1) },
        { XMFLOAT3( 0.5f,-0.5f, 0.5f), XMFLOAT4(1,1,1,1), XMFLOAT2(1,1), XMFLOAT3(1,0,0), XMFLOAT3(0,0,1) },
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

    // Load textures
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string exeDir(exePath);
        exeDir = exeDir.substr(0, exeDir.find_last_of("\\/") + 1);
        std::string texDir = exeDir + "textures\\";

        const char* texFiles[3] = {
            "Wood_Crate_001_basecolor.jpg",
            "Wood_Crate_001_normal.jpg",
            "Wood_Crate_001_ambientOcclusion.jpg",
        };

        // Create SRV descriptor heap (shader-visible)
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.NumDescriptors = 3;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = renderer.device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&renderer.srvHeap));
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create SRV heap");
            return true; // Non-fatal
        }

        UINT srvDescSize = renderer.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = renderer.srvHeap->GetCPUDescriptorHandleForHeapStart();

        // Fallback pixel data
        unsigned char whitePixel[4] = {255, 255, 255, 255};
        unsigned char normalPixel[4] = {128, 128, 255, 255};

        for (int i = 0; i < 3; i++) {
            std::string path = texDir + texFiles[i];
            int w, h, channels;
            unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);

            if (!pixels) {
                w = 1; h = 1;
                pixels = nullptr;
                LOG_INFO("Using fallback texture for %s", texFiles[i]);
            } else {
                LOG_INFO("Loaded texture: %s (%dx%d)", texFiles[i], w, h);
            }

            // Create default heap texture
            D3D12_HEAP_PROPERTIES defaultHeapProps = {};
            defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC texDesc = {};
            texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            texDesc.Width = w;
            texDesc.Height = h;
            texDesc.DepthOrArraySize = 1;
            texDesc.MipLevels = 1;
            texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            texDesc.SampleDesc.Count = 1;
            texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

            hr = renderer.device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE,
                &texDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&renderer.texResources[i]));
            if (FAILED(hr)) {
                if (pixels) stbi_image_free(pixels);
                continue;
            }

            // Upload via staging buffer
            UINT64 uploadSize = 0;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
            renderer.device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, nullptr, nullptr, &uploadSize);

            const unsigned char* srcData = pixels ? pixels : ((i == 1) ? normalPixel : whitePixel);
            UINT srcRowPitch = w * 4;

            ComPtr<ID3D12Resource> uploadBuf = CreateUploadBuffer(renderer.device.Get(), nullptr, uploadSize);
            if (uploadBuf) {
                void* mapped = nullptr;
                D3D12_RANGE readRange = {0, 0};
                uploadBuf->Map(0, &readRange, &mapped);
                // Copy row by row respecting alignment
                for (int row = 0; row < h; row++) {
                    memcpy((uint8_t*)mapped + row * footprint.Footprint.RowPitch,
                           srcData + row * srcRowPitch, srcRowPitch);
                }
                uploadBuf->Unmap(0, nullptr);

                // Record copy command
                renderer.commandAllocator->Reset();
                renderer.commandList->Reset(renderer.commandAllocator.Get(), nullptr);

                D3D12_TEXTURE_COPY_LOCATION dst = {};
                dst.pResource = renderer.texResources[i].Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;

                D3D12_TEXTURE_COPY_LOCATION src = {};
                src.pResource = uploadBuf.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = footprint;

                renderer.commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

                // Transition to shader resource
                D3D12_RESOURCE_BARRIER texBarrier = {};
                texBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                texBarrier.Transition.pResource = renderer.texResources[i].Get();
                texBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                texBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                texBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                renderer.commandList->ResourceBarrier(1, &texBarrier);

                renderer.commandList->Close();
                ID3D12CommandList* lists[] = { renderer.commandList.Get() };
                renderer.commandQueue->ExecuteCommandLists(1, lists);

                // Wait for upload
                renderer.fenceValue++;
                renderer.commandQueue->Signal(renderer.fence.Get(), renderer.fenceValue);
                if (renderer.fence->GetCompletedValue() < renderer.fenceValue) {
                    renderer.fence->SetEventOnCompletion(renderer.fenceValue, renderer.fenceEvent);
                    WaitForSingleObject(renderer.fenceEvent, INFINITE);
                }
            }

            if (pixels) stbi_image_free(pixels);

            // Create SRV
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;

            renderer.device->CreateShaderResourceView(renderer.texResources[i].Get(), &srvDesc, srvHandle);
            srvHandle.ptr += srvDescSize;
        }
        renderer.texturesLoaded = true;
    }

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
    ID3D12Resource** textures, uint32_t count,
    uint32_t width, uint32_t height,
    DXGI_FORMAT format)
{
    // Store swapchain format and recreate PSOs to match on first call.
    // The underlying D3D12 resources are typeless (for Vulkan interop), so
    // both RTVs and PSOs need an explicit typed format.
    if (renderer.swapchainFormat != format) {
        renderer.swapchainFormat = format;

        // Recreate PSOs with the actual swapchain format
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = renderer.cubeRootSignature.Get();
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

        // Cube input layout (5 elements for textured vertices)
        D3D12_INPUT_ELEMENT_DESC cubeInputElements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
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

        // Recreate textured cube PSO with cubeRootSignature if available
        if (renderer.cubeRootSignature) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC cubePsoDesc = psoDesc;
            cubePsoDesc.pRootSignature = renderer.cubeRootSignature.Get();
            cubePsoDesc.InputLayout = { cubeInputElements, _countof(cubeInputElements) };
            cubePsoDesc.VS = { cubeVS->GetBufferPointer(), cubeVS->GetBufferSize() };
            cubePsoDesc.PS = { cubePS->GetBufferPointer(), cubePS->GetBufferSize() };

            renderer.cubePSOTextured.Reset();
            hr = renderer.device->CreateGraphicsPipelineState(&cubePsoDesc, IID_PPV_ARGS(&renderer.cubePSOTextured));
            if (FAILED(hr)) {
                LOG_ERROR("Failed to recreate textured cube PSO with format 0x%X: 0x%08X", format, hr);
                // Non-fatal: fall back to non-textured
            }
        }

        // Grid input layout (uses simple grid root signature)
        D3D12_INPUT_ELEMENT_DESC gridInputElements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        psoDesc.pRootSignature = renderer.rootSignature.Get();
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

    // Create RTV descriptor heap
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = count;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HRESULT hr = renderer.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&renderer.rtvHeap));
        if (FAILED(hr)) return false;
        renderer.rtvDescriptorSize = renderer.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        renderer.rtvCount = 0;
    }

    // Create RTVs for swapchain images.
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

    // Create single depth buffer for SBS swapchain
    if (!renderer.depthBuffer) {
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        HRESULT hr = renderer.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&renderer.dsvHeap));
        if (FAILED(hr)) return false;
        renderer.dsvDescriptorSize = renderer.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

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

        hr = renderer.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
            IID_PPV_ARGS(&renderer.depthBuffer));
        if (FAILED(hr)) return false;

        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = renderer.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        renderer.device->CreateDepthStencilView(renderer.depthBuffer.Get(), nullptr, dsvHandle);
    }

    LOG_INFO("Created %u RTVs + depth (%ux%u)", count, width, height);
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
    uint32_t viewportX, uint32_t viewportY,
    uint32_t width, uint32_t height,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix,
    float zoomScale,
    bool clear
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

    // Set render targets
    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    // Clear only on first eye (clear==true)
    if (clear) {
        float clearColor[4] = {0.05f, 0.05f, 0.25f, 1.0f};
        cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            1.0f, 0, 0, nullptr);
    }

    // Set viewport and scissor with offset for SBS rendering
    D3D12_VIEWPORT viewport = {};
    viewport.TopLeftX = (FLOAT)viewportX;
    viewport.TopLeftY = (FLOAT)viewportY;
    viewport.Width = (FLOAT)width;
    viewport.Height = (FLOAT)height;
    viewport.MaxDepth = 1.0f;
    cmdList->RSSetViewports(1, &viewport);

    D3D12_RECT scissor = { (LONG)viewportX, (LONG)viewportY, (LONG)(viewportX + width), (LONG)(viewportY + height) };
    cmdList->RSSetScissorRects(1, &scissor);

    // Zoom in eye space: scale only x,y (not z) so perspective division doesn't
    // cancel the effect. Keeps the viewport center fixed on screen.
    XMMATRIX zoom = XMMatrixScaling(zoomScale, zoomScale, 1.0f);

    // Draw cube - base rests on grid at y=0
    {
        const float cubeSize = 0.06f;
        const float cubeHeight = cubeSize / 2.0f;
        XMMATRIX cubeScale = XMMatrixScaling(cubeSize, cubeSize, cubeSize);
        XMMATRIX cubeRot = XMMatrixRotationY(renderer.cubeRotation);
        XMMATRIX cubeTrans = XMMatrixTranslation(0.0f, cubeHeight, 0.0f);
        XMMATRIX cubeWVP = cubeRot * cubeScale * cubeTrans * viewMatrix * zoom * projMatrix;

        D3D12ConstantBuffer cb;
        XMStoreFloat4x4(&cb.worldViewProj, cubeWVP);
        cb.color = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        XMMATRIX cubeModel = cubeRot * cubeScale;
        XMStoreFloat4x4(&cb.model, cubeModel);

        if (renderer.texturesLoaded && renderer.cubePSOTextured) {
            cmdList->SetGraphicsRootSignature(renderer.cubeRootSignature.Get());
            cmdList->SetPipelineState(renderer.cubePSOTextured.Get());
            cmdList->SetGraphicsRoot32BitConstants(0, sizeof(cb) / 4, &cb, 0);
            ID3D12DescriptorHeap* heaps[] = { renderer.srvHeap.Get() };
            cmdList->SetDescriptorHeaps(1, heaps);
            cmdList->SetGraphicsRootDescriptorTable(1, renderer.srvHeap->GetGPUDescriptorHandleForHeapStart());
        } else {
            cmdList->SetGraphicsRootSignature(renderer.cubeRootSignature.Get());
            cmdList->SetPipelineState(renderer.cubePSO.Get());
            cmdList->SetGraphicsRoot32BitConstants(0, sizeof(cb) / 4, &cb, 0);
            if (renderer.srvHeap) {
                ID3D12DescriptorHeap* heaps[] = { renderer.srvHeap.Get() };
                cmdList->SetDescriptorHeaps(1, heaps);
                cmdList->SetGraphicsRootDescriptorTable(1, renderer.srvHeap->GetGPUDescriptorHandleForHeapStart());
            }
        }
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->IASetVertexBuffers(0, 1, &renderer.cubeVBV);
        cmdList->IASetIndexBuffer(&renderer.cubeIBV);
        cmdList->DrawIndexedInstanced(36, 1, 0, 0, 0);
    }

    // Draw grid
    {
        cmdList->SetGraphicsRootSignature(renderer.rootSignature.Get());

        const float gridScale = 0.05f;
        XMMATRIX gridWorld = XMMatrixScaling(gridScale, gridScale, gridScale) *
                             XMMatrixTranslation(0, gridScale, 0);
        XMMATRIX gridWVP = gridWorld * viewMatrix * zoom * projMatrix;

        D3D12ConstantBuffer cb;
        XMStoreFloat4x4(&cb.worldViewProj, gridWVP);
        cb.color = XMFLOAT4(0.3f, 0.3f, 0.35f, 1.0f);
        XMStoreFloat4x4(&cb.model, XMMatrixIdentity());

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

    renderer.cubePSOTextured.Reset();
    renderer.cubeRootSignature.Reset();
    renderer.srvHeap.Reset();
    for (int i = 0; i < 3; i++) renderer.texResources[i].Reset();

    renderer.fence.Reset();
    renderer.depthBuffer.Reset();
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
