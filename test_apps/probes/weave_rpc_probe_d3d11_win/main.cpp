// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_weave probe (#625) — native, browser-free Step-0 harness.
 *
 * A present-owner that exercises the window-bound synchronous weave service end
 * to end with zero browser:
 *
 *   1. Creates its own OS window + a transparent DirectComposition swap chain.
 *   2. Brings up a forced-IPC OpenXR session bound to that window
 *      (XR_DXR_win32_window_binding, transparent, no shared texture).
 *   3. xrWeaveBindWindowDXR(window) once for DP phase-snap.
 *   4. Builds a known pre-weave SBS test texture (left half red, right half blue)
 *      as a keyed-mutex shared texture.
 *   5. Per frame: xrWeaveSubmitDXR(sbs, windowRelativeRect) → weaved shared
 *      texture + fence; GPU-waits the fence; presents the weaved handback via its
 *      own DComp swap chain. Logs the per-frame round-trip latency.
 *
 * The DP does ALL weaving inside the runtime (ADR-007 / ADR-019). The weave is
 * confined to a centred sub-rect; outside the rect the output stays transparent,
 * so DComp shows the desktop and the sub-rect honouring is visible.
 *
 * Autonomous capture: `touch %TEMP%\weave_probe_trigger` dumps the weaved output
 * to %TEMP%\weave_probe_output.bmp (the weave doesn't run inside
 * multi_compositor_render, so the workspace screenshot trigger won't catch it).
 *
 * Run forced-IPC: set XRT_FORCE_MODE=ipc process-level, start displayxr-service,
 * then launch this exe.
 */

#include "xr_session.h"
#include "logging.h"

#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

using Microsoft::WRL::ComPtr;

// ---- Layout constants -------------------------------------------------------
static const uint32_t kWinW = 1280;
static const uint32_t kWinH = 720;
static const uint32_t kRectW = 640; //!< weaved sub-rect (per-view = kRectW)
static const uint32_t kRectH = 360;

// ---- Global D3D / window state ----------------------------------------------
static HWND g_hwnd = nullptr;
static bool g_quit = false;
static bool g_resized = false;

static ComPtr<ID3D11Device5> g_device;
static ComPtr<ID3D11DeviceContext4> g_context;

static ComPtr<IDCompositionDevice> g_dcompDevice;
static ComPtr<IDCompositionTarget> g_dcompTarget;
static ComPtr<IDCompositionVisual> g_dcompVisual;
static ComPtr<IDXGISwapChain1> g_swapChain;

// Pre-weave SBS input (keyed-mutex shared texture), re-rendered off-axis each
// frame from the runtime's returned tracked eyes (look-around).
static ComPtr<ID3D11Texture2D> g_sbsTex;
static ComPtr<ID3D11RenderTargetView> g_sbsRtv;
static ComPtr<IDXGIKeyedMutex> g_sbsMutex;
static HANDLE g_sbsHandle = nullptr; //!< NT handle passed to xrWeaveSubmitDXR
static uint32_t g_sbsW = 0, g_sbsH = 0; //!< window-sized SBS input (batch layout)
// Latest tracked per-eye horizontal position (metres, display space) returned by
// the previous weave_submit; drives this frame's off-axis parallax. [0]=L [1]=R.
static float g_lastEyeX[2] = {0.0f, 0.0f};
// DXR_WEAVE_V6=1 switches the input to the v6 N-view worst-case atlas layout
// (#774) instead of the v3/v4/v5 per-rect squeezed SBS. Opt-in so the default
// run still regression-covers the shipped path.
static bool g_useV6 = false;

// v4 overlay atlas (browser#18): a window-sized premultiplied-alpha 2D layer the
// DP composites OVER the woven output. Painted ONCE (a static crisp 2D badge that
// must stay put / show no parallax while the woven squares look around), submitted
// each frame via a chained XrWeaveSubmitOverlaysDXR. Validates the runtime v4 path
// end-to-end on real Leia hardware before the browser drives it.
static ComPtr<ID3D11Texture2D> g_overlayTex;
static ComPtr<ID3D11RenderTargetView> g_overlayRtv;
static ComPtr<IDXGIKeyedMutex> g_overlayMutex;
static HANDLE g_overlayHandle = nullptr; //!< NT handle chained on XrWeaveSubmitOverlaysDXR
static uint32_t g_overlayW = 0, g_overlayH = 0;

// Weaved output handback (opened from the runtime's exported handles).
static ComPtr<ID3D11Texture2D> g_weavedTex;
static ComPtr<ID3D11Fence> g_weaveFence;
static uint32_t g_weavedW = 0, g_weavedH = 0;

// ---- Win32 window -----------------------------------------------------------
static LRESULT CALLBACK
WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_CLOSE: g_quit = true; return 0;
	case WM_DESTROY: PostQuitMessage(0); return 0;
	case WM_SIZE:
		if (wParam != SIZE_MINIMIZED) {
			g_resized = true;
		}
		return 0;
	case WM_KEYDOWN:
		if (wParam == VK_ESCAPE) {
			g_quit = true;
		}
		return 0;
	default: return DefWindowProc(hwnd, msg, wParam, lParam);
	}
}

static bool
CreateAppWindow(HINSTANCE hInst)
{
	WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInst;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = L"DXRWeaveRpcProbe";
	RegisterClassExW(&wc);

	RECT r = {0, 0, (LONG)kWinW, (LONG)kWinH};
	AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_NOREDIRECTIONBITMAP);
	// WS_EX_NOREDIRECTIONBITMAP: required for a DComp-presented (alpha) window.
	g_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"DisplayXR Weave RPC Probe (#625)",
	                         WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
	                         nullptr, nullptr, hInst, nullptr);
	if (!g_hwnd) {
		LOG_ERROR("CreateWindowEx failed: %lu", GetLastError());
		return false;
	}
	ShowWindow(g_hwnd, SW_SHOW);
	return true;
}

// ---- D3D11 device on the OpenXR-required adapter -----------------------------
static bool
CreateDeviceOnAdapter(LUID luid)
{
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		LOG_ERROR("CreateDXGIFactory1 failed");
		return false;
	}
	ComPtr<IDXGIAdapter1> adapter, chosen;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 d = {};
		adapter->GetDesc1(&d);
		if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) {
			chosen = adapter;
			break;
		}
	}
	ComPtr<ID3D11Device> dev;
	ComPtr<ID3D11DeviceContext> ctx;
	D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; // DComp needs BGRA support
	HRESULT hr = D3D11CreateDevice(chosen.Get(), chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
	                               nullptr, flags, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
	if (FAILED(hr)) {
		LOG_ERROR("D3D11CreateDevice failed: 0x%08lx", hr);
		return false;
	}
	if (FAILED(dev.As(&g_device)) || FAILED(ctx.As(&g_context))) {
		LOG_ERROR("ID3D11Device5/Context4 not available");
		return false;
	}
	return true;
}

// ---- DirectComposition transparent swap chain (caller-owned present) ---------
static bool
CreateCompositionSwapChain(uint32_t w, uint32_t h)
{
	ComPtr<IDXGIDevice> dxgiDevice;
	g_device.As(&dxgiDevice);
	ComPtr<IDXGIAdapter> adapter;
	dxgiDevice->GetAdapter(&adapter);
	ComPtr<IDXGIFactory2> factory;
	adapter->GetParent(IID_PPV_ARGS(&factory));

	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = w;
	sd.Height = h;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 2;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
	HRESULT hr = factory->CreateSwapChainForComposition(g_device.Get(), &sd, nullptr, &g_swapChain);
	if (FAILED(hr)) {
		LOG_ERROR("CreateSwapChainForComposition failed: 0x%08lx", hr);
		return false;
	}

	if (!g_dcompDevice) {
		hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&g_dcompDevice));
		if (FAILED(hr)) {
			LOG_ERROR("DCompositionCreateDevice failed: 0x%08lx", hr);
			return false;
		}
		hr = g_dcompDevice->CreateTargetForHwnd(g_hwnd, TRUE, &g_dcompTarget);
		if (FAILED(hr)) {
			LOG_ERROR("CreateTargetForHwnd failed: 0x%08lx", hr);
			return false;
		}
		g_dcompDevice->CreateVisual(&g_dcompVisual);
	}
	g_dcompVisual->SetContent(g_swapChain.Get());
	g_dcompTarget->SetRoot(g_dcompVisual.Get());
	g_dcompDevice->Commit();
	return true;
}

// ---- Pre-weave SBS texture (keyed-mutex shared, render-target) ---------------
// v5 batch layout (browser#22): the input is WINDOW-sized with each element's
// squeezed SBS at its own window position (not an element-sized 2x1 atlas), so
// the runtime exercises the batch weave path + the firstChunk transparent clear.
// (Re)created at the client size on resize, like the overlay atlas.
static bool
EnsureSbsTexture(uint32_t w, uint32_t h)
{
	if (w == 0 || h == 0) {
		return false;
	}
	if (g_sbsTex && g_sbsW == w && g_sbsH == h) {
		return true; // still valid — the service caches the import by handle
	}
	g_sbsRtv.Reset();
	g_sbsMutex.Reset();
	g_sbsTex.Reset();
	if (g_sbsHandle != nullptr) {
		CloseHandle(g_sbsHandle);
		g_sbsHandle = nullptr;
	}

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; // RTV for per-frame ClearView
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_sbsTex))) {
		LOG_ERROR("SBS CreateTexture2D failed");
		return false;
	}
	if (FAILED(g_device->CreateRenderTargetView(g_sbsTex.Get(), nullptr, &g_sbsRtv))) {
		LOG_ERROR("SBS CreateRenderTargetView failed");
		return false;
	}
	if (FAILED(g_sbsTex.As(&g_sbsMutex))) {
		LOG_ERROR("SBS texture has no keyed mutex");
		return false;
	}
	ComPtr<IDXGIResource1> res1;
	if (FAILED(g_sbsTex.As(&res1)) ||
	    FAILED(res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
	                                    &g_sbsHandle))) {
		LOG_ERROR("SBS CreateSharedHandle failed");
		return false;
	}
	g_sbsW = w;
	g_sbsH = h;
	LOG_INFO("Pre-weave SBS render target ready (%ux%u window-sized, NT handle=%p)", w, h, g_sbsHandle);
	return true;
}

// ---- v4 overlay atlas (premul-RGBA, window-sized, keyed-mutex shared) ---------
// (Re)create the overlay atlas at the window client size and paint a static 2D
// badge (opaque magenta bar near the top) on a transparent field. The service
// composites it "over" the woven output, so the bar should read as crisp 2D at
// screen depth — no interlace, and NO parallax when the head moves (unlike the
// woven squares). Painted once per (re)size; the service re-composites each frame.
static bool
EnsureOverlayTexture(uint32_t w, uint32_t h)
{
	if (w == 0 || h == 0) {
		return false;
	}
	if (g_overlayTex && g_overlayW == w && g_overlayH == h) {
		return true; // still valid — the service caches the import by handle
	}
	g_overlayRtv.Reset();
	g_overlayMutex.Reset();
	g_overlayTex.Reset();
	if (g_overlayHandle != nullptr) {
		CloseHandle(g_overlayHandle);
		g_overlayHandle = nullptr;
	}

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // premultiplied alpha
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_overlayTex))) {
		LOG_ERROR("overlay CreateTexture2D failed");
		return false;
	}
	if (FAILED(g_device->CreateRenderTargetView(g_overlayTex.Get(), nullptr, &g_overlayRtv))) {
		LOG_ERROR("overlay CreateRenderTargetView failed");
		return false;
	}
	if (FAILED(g_overlayTex.As(&g_overlayMutex))) {
		LOG_ERROR("overlay texture has no keyed mutex");
		return false;
	}
	ComPtr<IDXGIResource1> res1;
	if (FAILED(g_overlayTex.As(&res1)) ||
	    FAILED(res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
	                                    &g_overlayHandle))) {
		LOG_ERROR("overlay CreateSharedHandle failed");
		return false;
	}

	// Paint once: transparent everywhere, one opaque magenta bar near the top.
	if (g_overlayMutex->AcquireSync(0, 1000) != S_OK) {
		LOG_ERROR("overlay AcquireSync failed");
		return false;
	}
	const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	const float magenta[4] = {0.90f, 0.05f, 0.90f, 1.0f}; // opaque; premul == straight when a=1
	g_context->ClearRenderTargetView(g_overlayRtv.Get(), transparent);
	D3D11_RECT barR = {(LONG)(w / 8), (LONG)(h / 12), (LONG)(w - w / 8), (LONG)(h / 12 + h / 12)};
	g_context->ClearView(g_overlayRtv.Get(), magenta, &barR, 1);
	g_context->Flush();
	g_overlayMutex->ReleaseSync(0);

	g_overlayW = w;
	g_overlayH = h;
	LOG_INFO("v4 overlay atlas ready (%ux%u, NT handle=%p) — magenta 2D bar", w, h, g_overlayHandle);
	return true;
}

// Render the pre-weave SBS pair off-axis for the two eyes (shader-free, via
// ClearView). Each view shows a near (red) + far (green) square on a gray bg;
// each square shifts horizontally by -k*eye.x, with the near square's k larger,
// so head motion produces depth-ordered parallax (look-around) and the L/R eye
// difference produces stereo disparity.
//
// v5 batch layout: render the element's squeezed SBS into the window-sized
// input at the element's own window position [rx,ry,rw,rh] — left view in the
// rect's left half [rx, rx+rw/2], right view in the right half. The rest of the
// window-sized input is a GAP: cleared to transparent (alpha 0), mirroring the
// browser (opaque element on transparency).
static void
RenderSbsLookAround(float eyeLx, float eyeRx, int32_t rx, int32_t ry, int32_t rw, int32_t rh)
{
	if (!g_sbsRtv || !g_sbsMutex) {
		return;
	}
	// Producer side of the keyed-mutex handshake (key 0 = "done writing"); the
	// service AcquireSync(0)s the same texture inside weave_submit. Must release
	// BEFORE weave_submit or the service's same-key acquire would block.
	if (g_sbsMutex->AcquireSync(0, 1000) != S_OK) {
		return;
	}
	const float gap[4] = {0.0f, 0.0f, 0.0f, 0.0f};       // transparent GAP (outside the element)
	const float bg[4] = {0.10f, 0.10f, 0.12f, 1.0f};     // opaque element background
	const float farCol[4] = {0.15f, 0.80f, 0.20f, 1.0f}; // green, "far"
	const float nearCol[4] = {0.90f, 0.20f, 0.15f, 1.0f}; // red, "near"
	const float kFar = 200.0f;  // px shift per metre of eye x  (small parallax)
	const float kNear = 900.0f; // larger parallax → reads as nearer
	const int32_t hw = rw / 2;  // per-view (squeezed) width within the rect
	const float eyeX[2] = {eyeLx, eyeRx};

	// Whole window transparent, then the opaque element rect on top.
	g_context->ClearView(g_sbsRtv.Get(), gap, nullptr, 0);
	D3D11_RECT elemR = {rx, ry, rx + rw, ry + rh};
	g_context->ClearView(g_sbsRtv.Get(), bg, &elemR, 1);

	for (int v = 0; v < 2; v++) {
		const int32_t ox = rx + v * hw; // this view's left edge within the rect
		// Far square, centred in the view half, parallax -kFar*eye.x.
		int32_t fcx = ox + hw / 2 + (int32_t)(-kFar * eyeX[v]);
		int32_t fcy = ry + rh / 2;
		D3D11_RECT farR = {fcx - 60, fcy - 90, fcx + 60, fcy + 90};
		g_context->ClearView(g_sbsRtv.Get(), farCol, &farR, 1);
		// Near square, centred, parallax -kNear*eye.x (moves more).
		int32_t ncx = ox + hw / 2 + (int32_t)(-kNear * eyeX[v]);
		int32_t ncy = ry + rh / 2;
		D3D11_RECT nearR = {ncx - 40, ncy - 60, ncx + 40, ncy + 60};
		g_context->ClearView(g_sbsRtv.Get(), nearCol, &nearR, 1);
	}

	g_context->Flush(); // ensure writes are submitted before the service reads
	g_sbsMutex->ReleaseSync(0);
}

// v6 N-view atlas layout (#774): render the SAME scene into a worst-case-sized
// multiview atlas instead of per-rect squeezed SBS — the layout every native
// handle app uses (ADR-010 / ADR-030).
//
// The atlas is deliberately allocated LARGER than this mode's packed region
// (2*cw wide vs the cw the 2x1 grid at scaleX 0.5 actually fills) so the
// runtime's crop-before-DP branch is the one under test; a caller whose
// worst-case mode happens to fill the atlas exactly gets the zero-copy branch
// instead. Tiles are packed CONTIGUOUSLY from the top-left at
// (content_w, content_h) — NOT at atlas_w/tile_columns.
//
// Per-view content is (rw*scaleX, rh*scaleY) at (rx*scaleX, ry*scaleY) inside
// its tile, so at scale (0.5, 1.0) this carries exactly the same pixel budget
// as the v5 path above — the woven result should be indistinguishable, which is
// the regression assertion for N=2.
static void
RenderNViewLookAround(float eyeLx,
                      float eyeRx,
                      int32_t rx,
                      int32_t ry,
                      int32_t rw,
                      int32_t rh,
                      uint32_t contentW,
                      uint32_t contentH)
{
	if (!g_sbsRtv || !g_sbsMutex) {
		return;
	}
	if (g_sbsMutex->AcquireSync(0, 1000) != S_OK) {
		return;
	}
	const float gap[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	const float bg[4] = {0.10f, 0.10f, 0.12f, 1.0f};
	const float farCol[4] = {0.15f, 0.80f, 0.20f, 1.0f};
	const float nearCol[4] = {0.90f, 0.20f, 0.15f, 1.0f};
	const float kFar = 200.0f;
	const float kNear = 900.0f;
	const float eyeX[2] = {eyeLx, eyeRx};

	// Window -> tile scale. Height is unscaled here (scaleY 1.0), width halves.
	const float sx = (float)contentW / (float)(g_sbsW / 2); // contentW / window width
	const float sy = (float)contentH / (float)g_sbsH;

	// Whole atlas transparent — including the dead space beyond the packed
	// region, which the runtime never reads but which must not be stale.
	g_context->ClearView(g_sbsRtv.Get(), gap, nullptr, 0);

	for (int v = 0; v < 2; v++) {
		// Tile origin at CONTENT stride, contiguous from the top-left.
		const int32_t tx = v * (int32_t)contentW;
		const int32_t ty = 0;
		// The element at its own window position, scaled into the tile.
		const int32_t ex = tx + (int32_t)(rx * sx);
		const int32_t ey = ty + (int32_t)(ry * sy);
		const int32_t ew = (int32_t)(rw * sx);
		const int32_t eh = (int32_t)(rh * sy);

		D3D11_RECT elemR = {ex, ey, ex + ew, ey + eh};
		g_context->ClearView(g_sbsRtv.Get(), bg, &elemR, 1);

		int32_t fcx = ex + ew / 2 + (int32_t)(-kFar * eyeX[v] * sx);
		int32_t fcy = ey + eh / 2;
		D3D11_RECT farR = {fcx - (int32_t)(60 * sx), fcy - (int32_t)(90 * sy), fcx + (int32_t)(60 * sx),
		                   fcy + (int32_t)(90 * sy)};
		g_context->ClearView(g_sbsRtv.Get(), farCol, &farR, 1);

		int32_t ncx = ex + ew / 2 + (int32_t)(-kNear * eyeX[v] * sx);
		int32_t ncy = ey + eh / 2;
		D3D11_RECT nearR = {ncx - (int32_t)(40 * sx), ncy - (int32_t)(60 * sy), ncx + (int32_t)(40 * sx),
		                    ncy + (int32_t)(60 * sy)};
		g_context->ClearView(g_sbsRtv.Get(), nearCol, &nearR, 1);
	}

	g_context->Flush();
	g_sbsMutex->ReleaseSync(0);
}

// ---- Open the runtime's exported weaved texture + fence ----------------------
static bool
OpenWeavedHandback(const XrWeaveOutputDXR &out)
{
	g_weavedTex.Reset();
	g_weaveFence.Reset();
	if (out.weavedTexture != nullptr) {
		HRESULT hr = g_device->OpenSharedResource1((HANDLE)out.weavedTexture, IID_PPV_ARGS(&g_weavedTex));
		CloseHandle((HANDLE)out.weavedTexture); // caller owns the duplicated handle
		if (FAILED(hr)) {
			LOG_ERROR("OpenSharedResource1(weaved) failed: 0x%08lx", hr);
			return false;
		}
	}
	if (out.fence != nullptr) {
		HRESULT hr = g_device->OpenSharedFence((HANDLE)out.fence, IID_PPV_ARGS(&g_weaveFence));
		CloseHandle((HANDLE)out.fence);
		if (FAILED(hr)) {
			LOG_ERROR("OpenSharedFence failed: 0x%08lx", hr);
			return false;
		}
	}
	g_weavedW = out.width;
	g_weavedH = out.height;
	LOG_INFO("Opened weaved handback: %ux%u (tex=%p fence=%p)", g_weavedW, g_weavedH, (void *)g_weavedTex.Get(),
	         (void *)g_weaveFence.Get());
	return true;
}

// ---- Autonomous capture: dump the weaved texture to a BMP on file trigger ----
static void
MaybeDumpWeaved()
{
	char trig[MAX_PATH], outp[MAX_PATH];
	const char *tmp = getenv("TEMP");
	if (!tmp) {
		tmp = "C:\\Temp";
	}
	snprintf(trig, sizeof(trig), "%s\\weave_probe_trigger", tmp);
	if (GetFileAttributesA(trig) == INVALID_FILE_ATTRIBUTES || !g_weavedTex) {
		return;
	}
	DeleteFileA(trig);
	snprintf(outp, sizeof(outp), "%s\\weave_probe_output.bmp", tmp);

	D3D11_TEXTURE2D_DESC td = {};
	g_weavedTex->GetDesc(&td);
	D3D11_TEXTURE2D_DESC sd = td;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.MiscFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ComPtr<ID3D11Texture2D> staging;
	if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &staging))) {
		return;
	}
	g_context->CopyResource(staging.Get(), g_weavedTex.Get());
	g_context->Flush();
	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
		return;
	}
	const uint32_t w = td.Width, h = td.Height;

	// v5 firstChunk assertion (browser#22): with the batch clear the woven output
	// must be OPAQUE (alpha≈255) inside the element tile and TRANSPARENT (alpha≈0)
	// in the gap outside it — the property that lets the browser present the woven
	// output whole-window over the page. The BMP below drops alpha (24-bit), so
	// sample it here: a gap pixel near the top-left corner vs the window centre
	// (the centred element). This validates MY clear + the DP alpha-native
	// passthrough on sim (Leia's own alpha is confirmed by David's eyeball).
	if (w > 20 && h > 20) {
		const uint8_t *pGap = (const uint8_t *)m.pData + (size_t)10 * m.RowPitch + (size_t)10 * 4;
		const uint8_t *pTile = (const uint8_t *)m.pData + (size_t)(h / 2) * m.RowPitch + (size_t)(w / 2) * 4;
		LOG_INFO("v5 alpha check: gap(10,10).a=%u (want ~0)  tile(%u,%u).a=%u (want ~255)", pGap[3], w / 2,
		         h / 2, pTile[3]);
	}

	const uint32_t rowBytes = w * 3;
	const uint32_t padded = (rowBytes + 3) & ~3u;
	const uint32_t imgSize = padded * h;
#pragma pack(push, 1)
	struct {
		uint16_t bfType;
		uint32_t bfSize;
		uint16_t r1, r2;
		uint32_t bfOff;
		uint32_t biSize;
		int32_t biW, biH;
		uint16_t biPlanes, biBpp;
		uint32_t biComp, biImg;
		int32_t biXppm, biYppm;
		uint32_t biClr, biImp;
	} hdr = {};
#pragma pack(pop)
	hdr.bfType = 0x4D42;
	hdr.bfOff = sizeof(hdr);
	hdr.bfSize = sizeof(hdr) + imgSize;
	hdr.biSize = 40;
	hdr.biW = (int32_t)w;
	hdr.biH = (int32_t)h; // bottom-up
	hdr.biPlanes = 1;
	hdr.biBpp = 24;
	hdr.biImg = imgSize;
	FILE *f = fopen(outp, "wb");
	if (f) {
		fwrite(&hdr, sizeof(hdr), 1, f);
		std::vector<uint8_t> row(padded, 0);
		for (int32_t y = (int32_t)h - 1; y >= 0; y--) {
			const uint8_t *src = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
			for (uint32_t x = 0; x < w; x++) {
				// R8G8B8A8 src -> BGR bmp
				row[x * 3 + 0] = src[x * 4 + 2];
				row[x * 3 + 1] = src[x * 4 + 1];
				row[x * 3 + 2] = src[x * 4 + 0];
			}
			fwrite(row.data(), padded, 1, f);
		}
		fclose(f);
		LOG_INFO("Dumped weaved output to %s (%ux%u)", outp, w, h);
	}
	g_context->Unmap(staging.Get(), 0);
}

// -----------------------------------------------------------------------------
int WINAPI
wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
	InitializeLogging("weave_rpc_probe_d3d11_win");
	LOG_INFO("=== XR_DXR_weave probe (#625) starting ===");

	{
		char buf[8] = {0};
		DWORD n = GetEnvironmentVariableA("DXR_WEAVE_V6", buf, (DWORD)sizeof(buf));
		g_useV6 = (n > 0 && n < sizeof(buf) && buf[0] == '1');
	}
	LOG_INFO("Input layout: %s", g_useV6 ? "v6 N-view worst-case atlas (#774)"
	                                      : "v3/v4/v5 per-rect squeezed SBS");

	XrSessionManager xr;
	if (!InitializeOpenXR(xr)) {
		return 1;
	}
	LUID luid = {};
	if (!GetD3D11GraphicsRequirements(xr, &luid)) {
		return 1;
	}
	if (!CreateAppWindow(hInst)) {
		return 1;
	}
	if (!CreateDeviceOnAdapter(luid)) {
		return 1;
	}
	if (!CreateCompositionSwapChain(kWinW, kWinH)) {
		return 1;
	}
	// SBS input is (re)created window-sized in the frame loop via EnsureSbsTexture.
	if (!CreateSession(xr, g_device.Get(), g_hwnd)) {
		return 1;
	}

	// Pump events until the session is running (PollEvents calls xrBeginSession
	// on READY).
	LOG_INFO("Waiting for session to start...");
	for (int i = 0; i < 2000 && !xr.sessionRunning && !g_quit; i++) {
		MSG msg;
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		PollEvents(xr);
		Sleep(2);
	}
	if (!xr.sessionRunning) {
		LOG_ERROR("Session never reached running state");
		return 1;
	}

	// Bind the present-owner window for DP phase-snap.
	XrResult br = g_pfnWeaveBindWindow(xr.session, (void *)g_hwnd);
	LogXrResult("xrWeaveBindWindowDXR", br);
	if (XR_FAILED(br)) {
		return 1;
	}

	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	double latSum = 0.0;
	uint32_t latCount = 0;
	uint64_t frame = 0;
	bool haveHandback = false;

	LOG_INFO("Entering weave loop (ESC / close to quit)...");
	while (!g_quit && !xr.exitRequested) {
		MSG msg;
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		PollEvents(xr);

		if (g_resized) {
			g_resized = false;
			RECT rc = {};
			GetClientRect(g_hwnd, &rc);
			uint32_t cw = (uint32_t)(rc.right - rc.left), ch = (uint32_t)(rc.bottom - rc.top);
			if (cw > 0 && ch > 0) {
				g_weavedTex.Reset();
				g_swapChain->ResizeBuffers(0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
				haveHandback = false; // force re-open of the resized weaved output
			}
		}

		// Centre the weaved sub-rect in the current client area.
		RECT rc = {};
		GetClientRect(g_hwnd, &rc);
		int32_t cw = rc.right - rc.left, ch = rc.bottom - rc.top;
		uint32_t rw = (uint32_t)min((int32_t)kRectW, cw);
		uint32_t rh = (uint32_t)min((int32_t)kRectH, ch);
		int32_t rx = (cw - (int32_t)rw) / 2;
		int32_t ry = (ch - (int32_t)rh) / 2;

		// v6 (#774) is opt-in via DXR_WEAVE_V6=1 so the default run keeps
		// exercising the shipped v3/v4/v5 batch path unchanged.
		const uint32_t v6ContentW = (uint32_t)(cw / 2); // window * scaleX (0.5)
		const uint32_t v6ContentH = (uint32_t)ch;       // window * scaleY (1.0)

		if (g_useV6) {
			// Worst-case-sized atlas: deliberately wider than this mode's packed
			// region (2*cw vs cw) so the runtime's crop branch is under test.
			if (!EnsureSbsTexture((uint32_t)cw * 2, (uint32_t)ch)) {
				Sleep(100);
				continue;
			}
			RenderNViewLookAround(g_lastEyeX[0], g_lastEyeX[1], rx, ry, (int32_t)rw, (int32_t)rh,
			                      v6ContentW, v6ContentH);
		} else {
			// v5 batch: the SBS input is window-sized with the element at its window
			// position, so the runtime takes the batch weave path + firstChunk clear.
			if (!EnsureSbsTexture((uint32_t)cw, (uint32_t)ch)) {
				Sleep(100);
				continue;
			}

			// Render this frame's pre-weave SBS pair off-axis from the eyes the
			// runtime returned LAST frame (look-around / virtual-camera motion), into
			// the element's window-relative rect (rest of the window = transparent gap).
			RenderSbsLookAround(g_lastEyeX[0], g_lastEyeX[1], rx, ry, (int32_t)rw, (int32_t)rh);
		}

		// v4: keep the overlay atlas sized to the window client area so the DP
		// composites the 2D badge 1:1 over the woven output.
		bool haveOverlay = EnsureOverlayTexture((uint32_t)cw, (uint32_t)ch);

		XrWeaveSubmitInfoDXR in = {XR_TYPE_WEAVE_SUBMIT_INFO_DXR};
		in.inputTexture = (void *)g_sbsHandle;
		in.inputIsDxgi = XR_FALSE;
		in.rect.offset.x = rx; // ignored on the batch path, kept for completeness
		in.rect.offset.y = ry;
		in.rect.extent.width = (int32_t)rw;
		in.rect.extent.height = (int32_t)rh;
		in.firstChunk = XR_TRUE; // single submit per frame → also the first: clears the woven output

		// v3 batch: one window-relative rect (element position). Switches the
		// runtime to the window-sized-input batch layout + the firstChunk clear.
		XrRect2Di batchRect = {{rx, ry}, {(int32_t)rw, (int32_t)rh}};
		XrWeaveSubmitRectsDXR rects = {XR_TYPE_WEAVE_SUBMIT_RECTS_DXR};
		rects.rectCount = 1;
		rects.rects = &batchRect;
		in.next = &rects;

		// v6 (#774): declare the N-view atlas layout. With this chained the rect
		// above degrades to a scope hint and the runtime skips the per-rect
		// unpack blits entirely.
		XrWeaveSubmitLayoutDXR lay = {XR_TYPE_WEAVE_SUBMIT_LAYOUT_DXR};
		if (g_useV6) {
			lay.viewCount = 2;
			lay.tileColumns = 2;
			lay.tileRows = 1;
			lay.contentViewWidth = v6ContentW;
			lay.contentViewHeight = v6ContentH;
			rects.next = &lay;
		}

		// Chain the 2D overlay atlas (browser#18 v4) so the DP composites it over
		// the woven 3D. rectCount 0 = composite the whole (mostly-transparent) atlas.
		XrWeaveSubmitOverlaysDXR ov = {XR_TYPE_WEAVE_SUBMIT_OVERLAYS_DXR};
		if (haveOverlay) {
			ov.overlayTexture = (void *)g_overlayHandle;
			ov.overlayIsDxgi = XR_FALSE;
			ov.rectCount = 0;
			ov.rects = nullptr;
			// Append, don't overwrite — the v6 layout may already sit at
			// rects.next (chain is rects -> layout -> overlays).
			if (g_useV6) {
				lay.next = &ov;
			} else {
				rects.next = &ov;
			}
		}

		XrWeaveOutputDXR out = {XR_TYPE_WEAVE_OUTPUT_DXR};

		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		XrResult sr = g_pfnWeaveSubmit(xr.session, &in, &out);
		QueryPerformanceCounter(&t1);

		if (XR_FAILED(sr)) {
			LogXrResult("xrWeaveSubmitDXR", sr);
			Sleep(100);
			continue;
		}

		double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
		latSum += ms;
		latCount++;

		// Feed the returned tracked eyes into next frame's off-axis render.
		if (out.eyesValid == XR_TRUE && out.eyeCount >= 2) {
			g_lastEyeX[0] = out.eyes[0].x;
			g_lastEyeX[1] = out.eyes[1].x;
		}

		if (!haveHandback || out.weavedTexture != nullptr) {
			if (!OpenWeavedHandback(out)) {
				Sleep(100);
				continue;
			}
			haveHandback = true;
		}

		// GPU-wait the service's signal, then present the weaved texture.
		if (g_weaveFence) {
			g_context->Wait(g_weaveFence.Get(), out.fenceValue);
		}
		if (g_weavedTex && g_swapChain) {
			ComPtr<ID3D11Texture2D> back;
			if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
				g_context->CopyResource(back.Get(), g_weavedTex.Get());
			}
			g_swapChain->Present(1, 0);
			if (g_dcompDevice) {
				g_dcompDevice->Commit();
			}
		}

		MaybeDumpWeaved();

		if ((frame % 120) == 0 && latCount > 0) {
			LOG_INFO("weave round-trip: last=%.3f ms, avg=%.3f ms (%u frames), out=%ux%u rect=%d,%d %ux%u "
			         "eyes(valid=%d track=%d n=%u L.x=%.4f R.x=%.4f)",
			         ms, latSum / latCount, latCount, out.width, out.height, rx, ry, rw, rh,
			         out.eyesValid, out.eyesTracking, out.eyeCount, g_lastEyeX[0], g_lastEyeX[1]);
		}
		frame++;
	}

	if (latCount > 0) {
		LOG_INFO("=== weave probe done: %u frames, avg round-trip %.3f ms ===", latCount, latSum / latCount);
	}
	CleanupOpenXR(xr);
	ShutdownLogging();
	return 0;
}
