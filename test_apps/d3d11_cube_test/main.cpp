// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
// Simple D3D11 spinning cube - no external dependencies

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// Window settings
static const wchar_t* WINDOW_CLASS = L"D3D11CubeTestClass";
static const wchar_t* WINDOW_TITLE = L"D3D11 Cube Test (No OpenXR)";
static const int WINDOW_WIDTH = 800;
static const int WINDOW_HEIGHT = 600;

// D3D11 objects
static ComPtr<ID3D11Device> g_device;
static ComPtr<ID3D11DeviceContext> g_context;
static ComPtr<IDXGISwapChain> g_swapChain;
static ComPtr<ID3D11RenderTargetView> g_rtv;
static ComPtr<ID3D11DepthStencilView> g_dsv;
static ComPtr<ID3D11VertexShader> g_vertexShader;
static ComPtr<ID3D11PixelShader> g_pixelShader;
static ComPtr<ID3D11InputLayout> g_inputLayout;
static ComPtr<ID3D11Buffer> g_vertexBuffer;
static ComPtr<ID3D11Buffer> g_indexBuffer;
static ComPtr<ID3D11Buffer> g_constantBuffer;
static ComPtr<ID3D11DepthStencilState> g_depthState;
static ComPtr<ID3D11RasterizerState> g_rasterizerState;

static float g_cubeRotation = 0.0f;
static bool g_running = true;

// Vertex structure
struct Vertex {
    XMFLOAT3 position;
    XMFLOAT4 color;
};

// Constant buffer
struct ConstantBuffer {
    XMFLOAT4X4 worldViewProj;
};

// Shader source
static const char* g_shaderSource = R"(
cbuffer Constants : register(b0) {
    float4x4 worldViewProj;
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
    output.position = mul(float4(input.position, 1.0), worldViewProj);
    output.color = input.color;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return input.color;
}
)";

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CLOSE:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            g_running = false;
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool InitD3D11(HWND hwnd) {
    // Create device and swap chain
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = WINDOW_WIDTH;
    scd.BufferDesc.Height = WINDOW_HEIGHT;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        featureLevels, 1, D3D11_SDK_VERSION,
        &scd, &g_swapChain, &g_device, &featureLevel, &g_context
    );
    if (FAILED(hr)) {
        MessageBox(hwnd, L"Failed to create D3D11 device", L"Error", MB_OK);
        return false;
    }

    // Create render target view
    ComPtr<ID3D11Texture2D> backBuffer;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    hr = g_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_rtv);
    if (FAILED(hr)) {
        MessageBox(hwnd, L"Failed to create RTV", L"Error", MB_OK);
        return false;
    }

    // Create depth buffer
    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = WINDOW_WIDTH;
    depthDesc.Height = WINDOW_HEIGHT;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    ComPtr<ID3D11Texture2D> depthBuffer;
    g_device->CreateTexture2D(&depthDesc, nullptr, &depthBuffer);
    g_device->CreateDepthStencilView(depthBuffer.Get(), nullptr, &g_dsv);

    // Compile shaders
    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
    hr = D3DCompile(g_shaderSource, strlen(g_shaderSource), nullptr, nullptr, nullptr,
        "VSMain", "vs_5_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            MessageBoxA(hwnd, (char*)errorBlob->GetBufferPointer(), "VS Compile Error", MB_OK);
        }
        return false;
    }

    hr = D3DCompile(g_shaderSource, strlen(g_shaderSource), nullptr, nullptr, nullptr,
        "PSMain", "ps_5_0", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            MessageBoxA(hwnd, (char*)errorBlob->GetBufferPointer(), "PS Compile Error", MB_OK);
        }
        return false;
    }

    g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vertexShader);
    g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_pixelShader);

    // Create input layout
    D3D11_INPUT_ELEMENT_DESC inputElements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    g_device->CreateInputLayout(inputElements, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_inputLayout);

    // Create cube vertices (colored)
    Vertex vertices[] = {
        // Front face (red)
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT4(1, 0, 0, 1) },
        { XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT4(1, 0, 0, 1) },
        { XMFLOAT3( 0.5f,  0.5f, -0.5f), XMFLOAT4(1, 0, 0, 1) },
        { XMFLOAT3( 0.5f, -0.5f, -0.5f), XMFLOAT4(1, 0, 0, 1) },
        // Back face (green)
        { XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT4(0, 1, 0, 1) },
        { XMFLOAT3( 0.5f, -0.5f,  0.5f), XMFLOAT4(0, 1, 0, 1) },
        { XMFLOAT3( 0.5f,  0.5f,  0.5f), XMFLOAT4(0, 1, 0, 1) },
        { XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT4(0, 1, 0, 1) },
        // Top face (blue)
        { XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT4(0, 0, 1, 1) },
        { XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT4(0, 0, 1, 1) },
        { XMFLOAT3( 0.5f,  0.5f,  0.5f), XMFLOAT4(0, 0, 1, 1) },
        { XMFLOAT3( 0.5f,  0.5f, -0.5f), XMFLOAT4(0, 0, 1, 1) },
        // Bottom face (yellow)
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT4(1, 1, 0, 1) },
        { XMFLOAT3( 0.5f, -0.5f, -0.5f), XMFLOAT4(1, 1, 0, 1) },
        { XMFLOAT3( 0.5f, -0.5f,  0.5f), XMFLOAT4(1, 1, 0, 1) },
        { XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT4(1, 1, 0, 1) },
        // Left face (magenta)
        { XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT4(1, 0, 1, 1) },
        { XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT4(1, 0, 1, 1) },
        { XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT4(1, 0, 1, 1) },
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT4(1, 0, 1, 1) },
        // Right face (cyan)
        { XMFLOAT3( 0.5f, -0.5f, -0.5f), XMFLOAT4(0, 1, 1, 1) },
        { XMFLOAT3( 0.5f,  0.5f, -0.5f), XMFLOAT4(0, 1, 1, 1) },
        { XMFLOAT3( 0.5f,  0.5f,  0.5f), XMFLOAT4(0, 1, 1, 1) },
        { XMFLOAT3( 0.5f, -0.5f,  0.5f), XMFLOAT4(0, 1, 1, 1) },
    };

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(vertices);
    vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData = { vertices };
    g_device->CreateBuffer(&vbDesc, &vbData, &g_vertexBuffer);

    // Create index buffer
    uint16_t indices[] = {
        0, 1, 2, 0, 2, 3,       // Front
        4, 5, 6, 4, 6, 7,       // Back
        8, 9, 10, 8, 10, 11,    // Top
        12, 13, 14, 12, 14, 15, // Bottom
        16, 17, 18, 16, 18, 19, // Left
        20, 21, 22, 20, 22, 23, // Right
    };

    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = sizeof(indices);
    ibDesc.Usage = D3D11_USAGE_IMMUTABLE;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibData = { indices };
    g_device->CreateBuffer(&ibDesc, &ibData, &g_indexBuffer);

    // Create constant buffer
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(ConstantBuffer);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_device->CreateBuffer(&cbDesc, nullptr, &g_constantBuffer);

    // Create depth stencil state
    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D11_COMPARISON_LESS;
    g_device->CreateDepthStencilState(&dsDesc, &g_depthState);

    // Create rasterizer state
    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_BACK;
    rsDesc.FrontCounterClockwise = FALSE;
    rsDesc.DepthClipEnable = TRUE;
    g_device->CreateRasterizerState(&rsDesc, &g_rasterizerState);

    return true;
}

void Render() {
    // Update rotation
    g_cubeRotation += 0.01f;

    // Build matrices
    XMMATRIX world = XMMatrixRotationY(g_cubeRotation) * XMMatrixRotationX(g_cubeRotation * 0.5f);
    XMMATRIX view = XMMatrixLookAtLH(
        XMVectorSet(0, 0, -3, 1),
        XMVectorSet(0, 0, 0, 1),
        XMVectorSet(0, 1, 0, 0)
    );
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)WINDOW_WIDTH / WINDOW_HEIGHT, 0.1f, 100.0f);
    XMMATRIX wvp = world * view * proj;

    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    g_context->Map(g_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    ConstantBuffer* cb = (ConstantBuffer*)mapped.pData;
    XMStoreFloat4x4(&cb->worldViewProj, XMMatrixTranspose(wvp));
    g_context->Unmap(g_constantBuffer.Get(), 0);

    // Clear and set render target
    float clearColor[] = { 0.1f, 0.1f, 0.2f, 1.0f };
    g_context->ClearRenderTargetView(g_rtv.Get(), clearColor);
    g_context->ClearDepthStencilView(g_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    g_context->OMSetRenderTargets(1, g_rtv.GetAddressOf(), g_dsv.Get());

    // Set viewport
    D3D11_VIEWPORT viewport = { 0, 0, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT, 0, 1 };
    g_context->RSSetViewports(1, &viewport);

    // Set pipeline state
    g_context->OMSetDepthStencilState(g_depthState.Get(), 0);
    g_context->RSSetState(g_rasterizerState.Get());
    g_context->IASetInputLayout(g_inputLayout.Get());
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    g_context->IASetVertexBuffers(0, 1, g_vertexBuffer.GetAddressOf(), &stride, &offset);
    g_context->IASetIndexBuffer(g_indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);

    g_context->VSSetShader(g_vertexShader.Get(), nullptr, 0);
    g_context->PSSetShader(g_pixelShader.Get(), nullptr, 0);
    g_context->VSSetConstantBuffers(0, 1, g_constantBuffer.GetAddressOf());

    // Draw
    g_context->DrawIndexed(36, 0, 0);

    // Present
    g_swapChain->Present(1, 0);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Register window class
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassEx(&wc);

    // Create window
    RECT rect = { 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowEx(0, WINDOW_CLASS, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        MessageBox(nullptr, L"Failed to create window", L"Error", MB_OK);
        return 1;
    }

    // Initialize D3D11
    if (!InitD3D11(hwnd)) {
        return 1;
    }

    // Show window
    ShowWindow(hwnd, nCmdShow);

    // Main loop
    MSG msg = {};
    while (g_running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Render();
    }

    return 0;
}
