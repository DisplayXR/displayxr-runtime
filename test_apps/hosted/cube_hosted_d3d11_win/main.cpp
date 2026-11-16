// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR - Standard OpenXR mode (DisplayXR creates window)
 *
 * This application demonstrates OpenXR without the XR_DXR_win32_window_binding extension.
 * DisplayXR will create its own window for rendering.
 *
 * Input is handled by DisplayXR's qwerty driver:
 * - WASD: Move camera
 * - Mouse drag: Look around
 * - ESC: Close window and exit
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wrl/client.h>

#include "d3d11_renderer.h"
#include "dxr_view_config.h"
#include "logging.h"
#include "xr_session.h"

#include <chrono>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Application name for logging
static const char* APP_NAME = "cube_hosted_d3d11_win";

// Global state
static bool g_running = true;

// ============================================================================
// Stylus mode (ADR-034 Amendment 8) — optional, `--stylus` / DXR_STYLUS=1.
//
// Binds /interaction_profiles/logitech/mx_ink_stylus_logitech
// (XR_LOGITECH_mx_ink_stylus_interaction) through the shared
// test_apps/common/dxr_stylus.h and draws the pen: the tip marker (grows with
// tip force), the barrel and the aim ray with the textured cube, plus a
// three-axis jack at the tip and the ray as lines in the pressure colour
// (green = hovering, through yellow to red, blue = docked). A rising edge of
// the front cluster click buzzes that hand. Drive it with the sim-stylus
// provider (DXR_INPUT_SIM_STYLUS_ENABLE=1) or any stylus input provider.
// ============================================================================

static bool g_stylusMode = false;
static dxr::Stylus g_stylus;
static dxr::StylusDraws g_stylusDraws;

static void StylusLogToApp(bool error, const char *msg)
{
    if (error) {
        LOG_ERROR("%s", msg);
    } else {
        LOG_INFO("%s", msg);
    }
}

//! The pen in one view: boxes with the shared textured cube, lines with the
//! renderer's grid shaders re-drawing the grid's first segment (no new pipeline).
static void DrawStylus(D3D11Renderer &renderer, ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv,
                       const XMMATRIX &viewMatrix, const XMMATRIX &projMatrix)
{
    const dxr::StylusDraws &sd = g_stylusDraws;
    for (int i = 0; i < sd.boxCount; i++) {
        XMFLOAT4X4 m4;
        memcpy(&m4, sd.boxes[i].model, sizeof(m4));
        XMFLOAT4X4 wvp;
        XMStoreFloat4x4(&wvp, XMLoadFloat4x4(&m4) * viewMatrix * projMatrix);
        RenderCubeWithMVP(renderer, rtv, dsv, &wvp.m[0][0]);
    }

    // The grid's first segment (displayxr-common's grid: i = -10 along Z).
    const float segA[3] = {-10.0f, -1.0f, -10.0f};
    const float segB[3] = {-10.0f, -1.0f, 10.0f};
    ID3D11DeviceContext *ctx = renderer.context.Get();
    UINT stride = sizeof(float) * 3, offset = 0;
    ctx->IASetInputLayout(renderer.gridInputLayout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ctx->IASetVertexBuffers(0, 1, renderer.gridVertexBuffer.GetAddressOf(), &stride, &offset);
    ctx->VSSetShader(renderer.gridVertexShader.Get(), nullptr, 0);
    ctx->PSSetShader(GridPixelShaderForTarget(renderer), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, renderer.constantBuffer.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, renderer.constantBuffer.GetAddressOf());
    for (int i = 0; i < sd.lineCount; i++) {
        const dxr::StylusLine &l = sd.lines[i];
        float m[16];
        dxr::StylusSegmentModel(segA, segB, l.p0, l.p1, m);
        XMFLOAT4X4 m4;
        memcpy(&m4, m, sizeof(m4));
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(ctx->Map(renderer.constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return;
        }
        ConstantBufferData *cb = (ConstantBufferData *)mapped.pData;
        XMStoreFloat4x4(&cb->worldViewProj, XMLoadFloat4x4(&m4) * viewMatrix * projMatrix);
        cb->color = XMFLOAT4(l.color[0], l.color[1], l.color[2], l.color[3]);
        XMStoreFloat4x4(&cb->model, XMMatrixIdentity());
        ctx->Unmap(renderer.constantBuffer.Get(), 0);
        ctx->Draw(2, 0);
    }
}

// Performance tracking
struct PerformanceStats {
    std::chrono::high_resolution_clock::time_point lastTime;
    float deltaTime = 0.0f;
    float fps = 0.0f;
    float frameTimeMs = 0.0f;
    int frameCount = 0;
    float fpsAccumulator = 0.0f;
};

static void UpdatePerformanceStats(PerformanceStats& stats) {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - stats.lastTime);
    stats.deltaTime = duration.count() / 1000000.0f;
    stats.frameTimeMs = duration.count() / 1000.0f;
    stats.lastTime = now;

    stats.fpsAccumulator += stats.deltaTime;
    stats.frameCount++;

    if (stats.fpsAccumulator >= 1.0f) {
        stats.fps = stats.frameCount / stats.fpsAccumulator;
        stats.frameCount = 0;
        stats.fpsAccumulator = 0.0f;
    }
}

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance;
    (void)hPrevInstance;
    (void)nCmdShow;

    // Initialize logging
    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR Cube OpenXR Application ===");
    LOG_INFO("OpenXR standard mode (DisplayXR creates window)");
    LOG_INFO("Input handled by DisplayXR's qwerty driver");

    // Initialize OpenXR
    LOG_INFO("Initializing OpenXR...");
    XrSessionManager xr = {};
    // Stylus mode: `--stylus` on the command line or DXR_STYLUS=1. The
    // extension is enabled at xrCreateInstance only when asked for.
    g_stylusMode = (lpCmdLine != nullptr && strstr(lpCmdLine, "--stylus") != nullptr) ||
                   dxr::StylusRequested(0, nullptr);
    g_stylusRequested = g_stylusMode;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        MessageBox(nullptr, L"Failed to initialize OpenXR", L"Error", MB_OK | MB_ICONERROR);
        ShutdownLogging();
        return 1;
    }

    // Get the required GPU adapter LUID from OpenXR
    LUID adapterLuid;
    if (!GetD3D11GraphicsRequirements(xr, &adapterLuid)) {
        LOG_ERROR("Failed to get D3D11 graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Initialize D3D11 on the correct adapter
    LOG_INFO("Initializing D3D11...");
    D3D11Renderer renderer = {};
    if (!InitializeD3D11WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D11 initialization failed");
        MessageBox(nullptr, L"Failed to initialize D3D11", L"Error", MB_OK | MB_ICONERROR);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create OpenXR session (DisplayXR creates window)
    LOG_INFO("Creating OpenXR session...");
    if (!CreateSession(xr, renderer.device.Get())) {
        LOG_ERROR("OpenXR session creation failed");
        MessageBox(nullptr, L"Failed to create OpenXR session", L"Error", MB_OK | MB_ICONERROR);
        CleanupD3D11(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create reference spaces
    // Hosted legacy-style cube: the cube is drawn at y = 1.6 - a legacy VR title
    // standing 1.6 m above the floor - so the app's world is STAGE (the floor),
    // not LOCAL (the head). Runtime #1370: xrLocateViews honours the requested
    // base space on both legs, and the hosted world-absolute concession that used
    // to lift LOCAL views to the standing height for these cubes is gone.
    if (!CreateSpaces(xr, XR_REFERENCE_SPACE_TYPE_STAGE)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }

    // Stylus mode: bind the MX Ink profile. Failure downgrades to the plain cube.
    if (g_stylusMode) {
        LOG_INFO("Stylus mode ENABLED (--stylus): logitech/mx_ink_stylus_logitech tip + aim + force + haptic");
        g_stylus.log = StylusLogToApp;
        if (!g_hasMxInkExt) {
            LOG_ERROR("stylus: XR_LOGITECH_mx_ink_stylus_interaction not available on this runtime");
            g_stylusMode = false;
        } else if (!dxr::StylusSetup(g_stylus, xr.instance, xr.session, XR_NULL_HANDLE)) {
            LOG_ERROR("stylus: setup failed - continuing without stylus mode");
            g_stylusMode = false;
        }
    }

    // Create single swapchain at native display resolution
    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }

    // Enumerate D3D11 swapchain images (now done per-app since common is API-agnostic)
    std::vector<XrSwapchainImageD3D11KHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u D3D11 swapchain images", count);
    }

    // Create single depth buffer at full swapchain dimensions
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> depthDSV;
    {
        ID3D11Texture2D* depthTex = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        if (!CreateDepthStencilView(renderer, xr.swapchain.width, xr.swapchain.height, &depthTex, &dsv)) {
            LOG_ERROR("Failed to create depth buffer");
            CleanupOpenXR(xr);
            CleanupD3D11(renderer);
            ShutdownLogging();
            return 1;
        }
        depthTexture.Attach(depthTex);
        depthDSV.Attach(dsv);
    }

    // Performance tracking
    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Rendering in DisplayXR's window (input via qwerty driver)");
    LOG_INFO("Controls: WASD=Move, QE=Up/Down, Mouse=Look, ESC=Quit");
    LOG_INFO("");

    // Main loop - no window, just process OpenXR frames
    // Exit when OpenXR session ends (user closes DisplayXR window or presses ESC)
    while (g_running && !xr.exitRequested) {
        // Update performance stats
        UpdatePerformanceStats(perfStats);

        // Update scene (cube rotation)
        UpdateScene(renderer, perfStats.deltaTime);

        // Poll OpenXR events
        PollEvents(xr);

        // Only render if session is running
        if (xr.sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(xr, frameState)) {
                // Stylus mode: sync + locate the pen in the views' space.
                if (g_stylusMode) {
                    dxr::StylusUpdate(g_stylus, xr.session, xr.localSpace, frameState.predictedDisplayTime,
                                      XR_NULL_HANDLE);
                    dxr::StylusBuildDraws(g_stylus, 1.0f, g_stylusDraws);
                }
                XrCompositionLayerProjectionView projectionViews[8] = {};
                uint32_t submitViewCount = 2;

                if (frameState.shouldRender) {
                    // Camera movement is handled by DisplayXR's qwerty driver
                    // Pass zeros for player transform - XR poses already include qwerty input
                    if (LocateViews(xr, frameState.predictedDisplayTime,
                        0.0f, 0.0f, 0.0f,  // playerPos (handled by qwerty)
                        0.0f, 0.0f)) {     // playerYaw/Pitch (handled by qwerty)

                        // Use current mode's view count (not xr.viewCount which is max across all modes)
                        uint32_t modeViewCount =
                            (xr.currentModeIndex < xr.renderingModeCount)
                                ? xr.renderingModeViewCounts
                                      [xr.currentModeIndex]
                                : xr.viewCount;

                        // Get raw view poses (pre-player-transform) for projection views.
                        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                        locateInfo.viewConfigurationType = xr.viewConfigType;
                        locateInfo.displayTime = frameState.predictedDisplayTime;
                        locateInfo.space = xr.localSpace;

                        XrViewState viewState = {XR_TYPE_VIEW_STATE};
                        uint32_t rawViewCount = 8;
                        XrView rawViews[8];
                        for (uint32_t i = 0; i < 8; i++) rawViews[i] = {XR_TYPE_VIEW};
                        xrLocateViews(xr.session, &locateInfo, &viewState, 8, &rawViewCount, rawViews);

                        // ADR-041: the layer carries the LOCATED count, not the
                        // active mode's — only modeViewCount of them are
                        // rendered, and the tail is aliased onto view 0 below.
                        uint32_t locatedCount =
                            (rawViewCount > 0) ? rawViewCount : modeViewCount;
                        if (locatedCount > 8)
                          locatedCount = 8; // projectionViews[] capacity
                        if (modeViewCount > locatedCount)
                          modeViewCount = locatedCount;
                        submitViewCount = locatedCount;

                        // Get tile layout from rendering mode, with fallback
                        uint32_t tileColumns = (xr.currentModeIndex < xr.renderingModeCount)
                            ? xr.renderingModeTileColumns[xr.currentModeIndex] : (modeViewCount >= 2 ? 2 : 1);
                        uint32_t tileRows = (xr.currentModeIndex < xr.renderingModeCount)
                            ? xr.renderingModeTileRows[xr.currentModeIndex] : ((modeViewCount + tileColumns - 1) / tileColumns);

                        uint32_t maxTileW = tileColumns > 0 ? xr.swapchain.width / tileColumns : xr.swapchain.width;
                        uint32_t maxTileH = tileRows > 0 ? xr.swapchain.height / tileRows : xr.swapchain.height;
                        uint32_t tileW = (xr.displayPixelWidth > 0)
                            ? (uint32_t)(xr.displayPixelWidth * xr.recommendedViewScaleX) : maxTileW;
                        uint32_t tileH = (xr.displayPixelHeight > 0)
                            ? (uint32_t)(xr.displayPixelHeight * xr.recommendedViewScaleY) : maxTileH;
                        if (tileW > maxTileW) tileW = maxTileW;
                        if (tileH > maxTileH) tileH = maxTileH;

                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(xr, imageIndex)) {
                            ID3D11Texture2D* swapchainTexture = swapchainImages[imageIndex].texture;

                            ID3D11RenderTargetView* rtv = nullptr;
                            CreateRenderTargetView(renderer, swapchainTexture,
                                static_cast<DXGI_FORMAT>(xr.swapchain.format), &rtv);

                            // Clear entire color+depth once before eye loop
                            float clearColor[4] = {0.05f, 0.05f, 0.25f, 1.0f};
                            ClearRenderTargetViewDisplayReferred(renderer, rtv, clearColor);
                            renderer.context->ClearDepthStencilView(depthDSV.Get(),
                                D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

                            for (uint32_t eye = 0; eye < modeViewCount; eye++) {
                                uint32_t tileX = eye % tileColumns;
                                uint32_t tileY = eye / tileColumns;

                                D3D11_VIEWPORT vp = {};
                                vp.TopLeftX = (FLOAT)(tileX * tileW);
                                vp.TopLeftY = (FLOAT)(tileY * tileH);
                                vp.Width = (FLOAT)tileW;
                                vp.Height = (FLOAT)tileH;
                                vp.MaxDepth = 1.0f;
                                renderer.context->RSSetViewports(1, &vp);

                                XMMATRIX viewMatrix = xr.viewMatrices[eye];
                                XMMATRIX projMatrix = xr.projMatrices[eye];

                                // Non-ext app: 0.3m cube at z=-2m, no zoom control
                                RenderScene(renderer, rtv, depthDSV.Get(),
                                    tileW, tileH,
                                    viewMatrix, projMatrix,
                                    1.0f, 1.6f, -2.0f, 0.3f);
                                if (g_stylusMode) {
                                    DrawStylus(renderer, rtv, depthDSV.Get(), viewMatrix, projMatrix);
                                }

                                // Set up projection view for this eye
                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {
                                    (int32_t)(tileX * tileW), (int32_t)(tileY * tileH)
                                };
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)tileW,
                                    (int32_t)tileH
                                };
                                projectionViews[eye].subImage.imageArrayIndex = 0;

                                projectionViews[eye].pose = rawViews[eye].pose;
                                projectionViews[eye].fov = rawViews[eye].fov;
                            }

                            // ADR-041: fill the inactive tail
                            // [modeViewCount, locatedCount) — own located
                            // pose/fov, view 0's subimage.
                            DxrAliasInactiveViews(projectionViews, rawViews,
                                                  locatedCount, modeViewCount);

                            if (rtv) rtv->Release();
                            ReleaseSwapchainImage(xr);
                        }
                    }
                }

                if (frameState.shouldRender) {
                    // Submit frame (projection layer only)
                    EndFrame(xr, frameState.predictedDisplayTime, projectionViews, submitViewCount);
                } else {
                    // Not visible: xrEndFrame is still required to keep the frame
                    // loop paced, but with NO layers — the projection views above
                    // were never located and would be rejected (POSE_INVALID).
                    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                    endInfo.displayTime = frameState.predictedDisplayTime;
                    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    endInfo.layerCount = 0;
                    endInfo.layers = nullptr;
                    xrEndFrame(xr.session, &endInfo);
                }
            }
        } else {
            Sleep(100);
        }
    }

    // Cleanup
    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    depthDSV.Reset();
    depthTexture.Reset();

    CleanupOpenXR(xr);
    CleanupD3D11(renderer);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
