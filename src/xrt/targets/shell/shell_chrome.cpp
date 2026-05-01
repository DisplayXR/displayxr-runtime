// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Shell-side chrome render module (Phase 2.C) implementation.
 *
 * C3.B initial: solid-color frosted-glass pill rendered into the chrome
 * swapchain image[0] on connect. Subsequent C3 sub-steps add buttons,
 * grip dots, app icon, glyphs, focus rim glow, hover-fade — all via the
 * same author-image-then-resubmit-layout pattern.
 */

#include "shell_chrome.h"
#include "shell_openxr.h"

#define WIN32_LEAN_AND_MEAN
#include <Unknwn.h>
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_spatial_workspace.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#define P(...)  std::printf(__VA_ARGS__)
#define PE(...) std::fprintf(stderr, __VA_ARGS__)

using Microsoft::WRL::ComPtr;

namespace {

// Pill geometry constants — mirror the runtime's in-process pill so the
// controller-rendered pill lands at the same place + size as the existing
// in-runtime chrome. Once C3.C adds full visual parity these stay; C5
// deletes the runtime-side mirror.
constexpr float PILL_W_FRAC      = 0.75f;   // pill width as a fraction of window width
constexpr float PILL_HEIGHT_M    = 0.008f;  // 8 mm tall — UI_TITLE_BAR_H_M default
constexpr float PILL_GAP_FRAC    = 0.5f;    // gap above content = pill height * gap_frac
constexpr uint32_t CHROME_TEX_W  = 512;     // 512×64 sRGB image for the pill
constexpr uint32_t CHROME_TEX_H  = 64;

constexpr int64_t DXGI_FORMAT_R8G8B8A8_UNORM_SRGB_VAL = 29; // matches DXGI_FORMAT_R8G8B8A8_UNORM_SRGB

struct chrome_slot
{
	XrWorkspaceClientId id;
	XrSwapchain         swapchain;
	float               win_w_m;
	float               win_h_m;
	bool                rendered_once;

	// D3D11 resources for rendering into the chrome image. We hold a strong
	// ref to image[0]'s texture and an RTV onto it. Released on disconnect.
	ComPtr<ID3D11Texture2D>        texture;
	ComPtr<ID3D11RenderTargetView> rtv;
};

} // namespace

// Constant buffer for the rounded-pill shader. Layout matches the HLSL cbuffer
// at register b0. Sized to a multiple of 16 bytes for D3D11.
struct PillCB
{
	float pill_size_m[2];     // pill width/height in METERS (used as SDF space)
	float corner_radius_m;    // corner radius in METERS (full pill = pill_h_m * 0.5)
	float _pad0;
	float fill_color[4];      // straight RGBA — alpha is the base pill opacity
};
static_assert(sizeof(PillCB) % 16 == 0, "PillCB must be 16-byte aligned");

struct shell_chrome
{
	struct shell_openxr_state *xr;
	ID3D11Device              *device;          // not owned
	ID3D11DeviceContext       *context;         // not owned

	std::vector<chrome_slot> slots;

	// Resolved at create.
	PFN_xrEnumerateSwapchainImages enum_images;

	// Shader pipeline for the rounded-pill render. Compiled once at create.
	ComPtr<ID3D11VertexShader>   vs_pill;
	ComPtr<ID3D11PixelShader>    ps_pill;
	ComPtr<ID3D11Buffer>         cb_pill;
	ComPtr<ID3D11RasterizerState> rs_state;
	ComPtr<ID3D11BlendState>     bs_passthrough;  // overwrite RTV — controller owns the chrome image
	ComPtr<ID3D11DepthStencilState> dss_disabled;
};

namespace {

const char *
xr_result_str(XrResult r)
{
	switch (r) {
	case XR_SUCCESS: return "XR_SUCCESS";
	case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
	case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
	case XR_ERROR_FEATURE_UNSUPPORTED: return "XR_ERROR_FEATURE_UNSUPPORTED";
	case XR_ERROR_HANDLE_INVALID: return "XR_ERROR_HANDLE_INVALID";
	default: return "(other)";
	}
}

chrome_slot *
find_slot(shell_chrome *sc, XrWorkspaceClientId id)
{
	for (auto &s : sc->slots) {
		if (s.id == id) return &s;
	}
	return nullptr;
}

void
release_slot_resources(chrome_slot &slot, shell_chrome *sc)
{
	if (slot.swapchain != XR_NULL_HANDLE) {
		XrResult r = sc->xr->destroy_chrome_swapchain(slot.swapchain);
		if (XR_FAILED(r)) {
			PE("shell_chrome: destroy_chrome_swapchain failed: %s\n", xr_result_str(r));
		}
		slot.swapchain = XR_NULL_HANDLE;
	}
	slot.rtv.Reset();
	slot.texture.Reset();
}

// SDF-based rounded-pill HLSL. Vertex shader emits a unit quad from
// SV_VertexID (4-vertex triangle strip). Pixel shader rasterizes the
// rounded rectangle in PILL-SPACE METERS (passed via cbuffer) so the
// corner ends are circular regardless of how the chrome image is
// stretched onto the pill quad. fwidth() gives derivative-based 1-pixel
// AA at whatever the actual rasterization scale is.
const char *PILL_SHADER_HLSL = R"(
struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VS(uint vid : SV_VertexID)
{
    // 4-vertex triangle strip:  (0,1) (0,0) (1,1) (1,0)  → covers [0,1]^2 UV
    float2 uvs[4] = { float2(0,1), float2(0,0), float2(1,1), float2(1,0) };
    VSOut o;
    o.uv  = uvs[vid];
    o.pos = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);
    return o;
}

cbuffer PillCB : register(b0)
{
    float2 pill_size_m;     // pill width/height in METERS
    float  corner_radius_m; // corner radius in METERS
    float  _pad0;
    float4 fill_color;      // straight RGBA
};

float4 PS(VSOut input) : SV_Target
{
    // Map UV to pill-space meters so the SDF is correct under arbitrary
    // image-to-quad stretch.
    float2 p = input.uv * pill_size_m;
    float2 center = pill_size_m * 0.5;
    float2 half_dim = pill_size_m * 0.5 - corner_radius_m;
    float2 d = abs(p - center) - half_dim;
    float dist = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - corner_radius_m;

    // Derivative-based 1-pixel AA at the actual rasterization density.
    float aa = max(fwidth(dist), 1e-6);
    float coverage = saturate(0.5 - dist / aa);
    return float4(fill_color.rgb, fill_color.a * coverage);
}
)";

bool
compile_shader_blob(const char *src, const char *entry, const char *target,
                    ComPtr<ID3DBlob> &out_blob)
{
	ComPtr<ID3DBlob> err;
	HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
	                        entry, target,
	                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
	                        out_blob.GetAddressOf(), err.GetAddressOf());
	if (FAILED(hr)) {
		PE("shell_chrome: shader compile (%s/%s) failed: 0x%08lx — %s\n",
		   entry, target, hr, err ? (const char *)err->GetBufferPointer() : "(no log)");
		return false;
	}
	return true;
}

bool
init_shader_pipeline(shell_chrome *sc)
{
	ComPtr<ID3DBlob> vs_blob, ps_blob;
	if (!compile_shader_blob(PILL_SHADER_HLSL, "VS", "vs_5_0", vs_blob)) return false;
	if (!compile_shader_blob(PILL_SHADER_HLSL, "PS", "ps_5_0", ps_blob)) return false;

	HRESULT hr = sc->device->CreateVertexShader(vs_blob->GetBufferPointer(),
	                                            vs_blob->GetBufferSize(),
	                                            nullptr, sc->vs_pill.GetAddressOf());
	if (FAILED(hr)) { PE("shell_chrome: CreateVertexShader failed: 0x%08lx\n", hr); return false; }

	hr = sc->device->CreatePixelShader(ps_blob->GetBufferPointer(),
	                                    ps_blob->GetBufferSize(),
	                                    nullptr, sc->ps_pill.GetAddressOf());
	if (FAILED(hr)) { PE("shell_chrome: CreatePixelShader failed: 0x%08lx\n", hr); return false; }

	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = sizeof(PillCB);
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = sc->device->CreateBuffer(&bd, nullptr, sc->cb_pill.GetAddressOf());
	if (FAILED(hr)) { PE("shell_chrome: CreateBuffer(cb_pill) failed: 0x%08lx\n", hr); return false; }

	D3D11_RASTERIZER_DESC rsd = {};
	rsd.FillMode = D3D11_FILL_SOLID;
	rsd.CullMode = D3D11_CULL_NONE;
	rsd.DepthClipEnable = TRUE;
	hr = sc->device->CreateRasterizerState(&rsd, sc->rs_state.GetAddressOf());
	if (FAILED(hr)) { PE("shell_chrome: CreateRasterizerState failed: 0x%08lx\n", hr); return false; }

	// Passthrough blend: overwrite the chrome image. The controller owns
	// every pixel of the chrome image; the runtime composites the image
	// over the atlas with its own alpha blend. We don't want any blending
	// inside the chrome image authoring step.
	D3D11_BLEND_DESC bsd = {};
	bsd.RenderTarget[0].BlendEnable = FALSE;
	bsd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	hr = sc->device->CreateBlendState(&bsd, sc->bs_passthrough.GetAddressOf());
	if (FAILED(hr)) { PE("shell_chrome: CreateBlendState failed: 0x%08lx\n", hr); return false; }

	D3D11_DEPTH_STENCIL_DESC dsd = {};
	dsd.DepthEnable = FALSE;
	dsd.StencilEnable = FALSE;
	hr = sc->device->CreateDepthStencilState(&dsd, sc->dss_disabled.GetAddressOf());
	if (FAILED(hr)) { PE("shell_chrome: CreateDepthStencilState failed: 0x%08lx\n", hr); return false; }

	return true;
}

// Render the floating-pill design into the chrome image[0]. C3.C-1: rounded
// frosted-blue pill via SDF pixel shader. Subsequent C3.C steps add grip
// dots, buttons, icon, glyphs, focus rim, hover-fade.
void
render_pill(shell_chrome *sc, chrome_slot &slot)
{
	if (!slot.rtv || !sc->vs_pill || !sc->ps_pill) {
		return;
	}

	// Frosted-blue pill — matches the in-runtime chrome's color so the
	// controller-rendered version is visually consistent at the C3.C-1
	// checkpoint. Alpha 0.7 = semi-transparent so workspace content shows
	// through the pill.
	const float pill_w_m = slot.win_w_m * PILL_W_FRAC;
	const float pill_h_m = PILL_HEIGHT_M;
	const float corner_r = pill_h_m * 0.5f; // full pill — half-circle ends

	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(sc->context->Map(sc->cb_pill.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		return;
	}
	auto *cb = static_cast<PillCB *>(mapped.pData);
	cb->pill_size_m[0] = pill_w_m;
	cb->pill_size_m[1] = pill_h_m;
	cb->corner_radius_m = corner_r;
	cb->_pad0 = 0.0f;
	cb->fill_color[0] = 0.20f;
	cb->fill_color[1] = 0.22f;
	cb->fill_color[2] = 0.28f;
	cb->fill_color[3] = 0.70f;
	sc->context->Unmap(sc->cb_pill.Get(), 0);

	// Clear to fully transparent so the SDF coverage outside the pill
	// leaves transparent pixels (the runtime composites the chrome image
	// with alpha blending — transparent regions show the underlying
	// content quad).
	const float clear_transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	sc->context->ClearRenderTargetView(slot.rtv.Get(), clear_transparent);

	ID3D11RenderTargetView *rtvs[] = {slot.rtv.Get()};
	sc->context->OMSetRenderTargets(1, rtvs, nullptr);
	sc->context->OMSetBlendState(sc->bs_passthrough.Get(), nullptr, 0xFFFFFFFF);
	sc->context->OMSetDepthStencilState(sc->dss_disabled.Get(), 0);
	sc->context->RSSetState(sc->rs_state.Get());

	D3D11_VIEWPORT vp = {};
	vp.Width = (float)CHROME_TEX_W;
	vp.Height = (float)CHROME_TEX_H;
	vp.MaxDepth = 1.0f;
	sc->context->RSSetViewports(1, &vp);

	D3D11_RECT scissor = {0, 0, (LONG)CHROME_TEX_W, (LONG)CHROME_TEX_H};
	sc->context->RSSetScissorRects(1, &scissor);

	sc->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	sc->context->IASetInputLayout(nullptr);
	sc->context->VSSetShader(sc->vs_pill.Get(), nullptr, 0);
	sc->context->PSSetShader(sc->ps_pill.Get(), nullptr, 0);
	sc->context->VSSetConstantBuffers(0, 1, sc->cb_pill.GetAddressOf());
	sc->context->PSSetConstantBuffers(0, 1, sc->cb_pill.GetAddressOf());

	sc->context->Draw(4, 0);

	// Flush so the GPU work is submitted before xrReleaseSwapchainImage
	// releases the keyed mutex — the runtime's chrome composite (on a
	// different D3D11 device) reads via the shared NT handle and would
	// otherwise sample stale or uninitialized memory.
	sc->context->Flush();
}

// Build the chrome layout struct: pose-in-client (above the window content),
// size (75% width × pill height), hit regions for grip + buttons (placeholder
// for C4 — geometry only, no interactivity yet).
void
push_layout(shell_chrome *sc, chrome_slot &slot)
{
	const float pill_w_m = slot.win_w_m * PILL_W_FRAC;
	const float pill_h_m = PILL_HEIGHT_M;
	const float gap_m    = pill_h_m * PILL_GAP_FRAC;
	const float pill_cy  = (slot.win_h_m * 0.5f) + gap_m + (pill_h_m * 0.5f);

	XrWorkspaceChromeLayoutEXT layout = {XR_TYPE_WORKSPACE_CHROME_LAYOUT_EXT};
	layout.poseInClient.orientation.x = 0.0f;
	layout.poseInClient.orientation.y = 0.0f;
	layout.poseInClient.orientation.z = 0.0f;
	layout.poseInClient.orientation.w = 1.0f;
	layout.poseInClient.position.x = 0.0f;
	layout.poseInClient.position.y = pill_cy;
	layout.poseInClient.position.z = 0.0f;
	layout.sizeMeters.width = pill_w_m;
	layout.sizeMeters.height = pill_h_m;
	layout.followsWindowOrient = XR_TRUE;
	layout.hitRegionCount = 0;
	layout.hitRegions = nullptr;
	layout.depthBiasMeters = 0.0f; // 0 = use runtime default

	XrResult r = sc->xr->set_chrome_layout(sc->xr->session, slot.id, &layout);
	if (XR_FAILED(r)) {
		PE("shell_chrome: set_chrome_layout(client=%u) failed: %s\n", slot.id, xr_result_str(r));
	}
}

bool
init_slot_d3d11(shell_chrome *sc, chrome_slot &slot)
{
	uint32_t image_count = 0;
	XrResult r = sc->enum_images(slot.swapchain, 0, &image_count, nullptr);
	if (XR_FAILED(r) || image_count == 0) {
		PE("shell_chrome: xrEnumerateSwapchainImages count failed: %s (count=%u)\n",
		   xr_result_str(r), image_count);
		return false;
	}

	std::vector<XrSwapchainImageD3D11KHR> images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
	r = sc->enum_images(slot.swapchain, image_count, &image_count,
	                    reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data()));
	if (XR_FAILED(r)) {
		PE("shell_chrome: xrEnumerateSwapchainImages fetch failed: %s\n", xr_result_str(r));
		return false;
	}

	// Single-image chrome swapchain — image[0] is the only thing we render.
	slot.texture.Attach(images[0].texture);
	slot.texture->AddRef(); // OpenXR returned a borrowed pointer; we addref to keep alive
	images[0].texture = nullptr;

	D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
	rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	rtv_desc.Texture2D.MipSlice = 0;
	HRESULT hr = sc->device->CreateRenderTargetView(slot.texture.Get(), &rtv_desc, slot.rtv.GetAddressOf());
	if (FAILED(hr)) {
		PE("shell_chrome: CreateRenderTargetView failed: 0x%08lx\n", hr);
		return false;
	}
	return true;
}

bool
acquire_render_release(shell_chrome *sc, chrome_slot &slot)
{
	XrSwapchainImageAcquireInfo aci = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	uint32_t index = 0;
	XrResult r = xrAcquireSwapchainImage(slot.swapchain, &aci, &index);
	if (XR_FAILED(r)) {
		PE("shell_chrome: xrAcquireSwapchainImage failed: %s\n", xr_result_str(r));
		return false;
	}

	XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wi.timeout = XR_INFINITE_DURATION;
	r = xrWaitSwapchainImage(slot.swapchain, &wi);
	if (XR_FAILED(r)) {
		PE("shell_chrome: xrWaitSwapchainImage failed: %s\n", xr_result_str(r));
		return false;
	}

	render_pill(sc, slot);

	XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	r = xrReleaseSwapchainImage(slot.swapchain, &ri);
	if (XR_FAILED(r)) {
		PE("shell_chrome: xrReleaseSwapchainImage failed: %s\n", xr_result_str(r));
		return false;
	}
	return true;
}

} // namespace

extern "C" struct shell_chrome *
shell_chrome_create(struct shell_openxr_state *xr)
{
	if (xr == nullptr || xr->d3d11_device == nullptr || xr->d3d11_context == nullptr) {
		PE("shell_chrome_create: missing D3D11 device/context\n");
		return nullptr;
	}

	auto *sc = new shell_chrome();
	sc->xr = xr;
	sc->device = static_cast<ID3D11Device *>(xr->d3d11_device);
	sc->context = static_cast<ID3D11DeviceContext *>(xr->d3d11_context);

	XrResult r = xrGetInstanceProcAddr(xr->instance, "xrEnumerateSwapchainImages",
	                                    reinterpret_cast<PFN_xrVoidFunction *>(&sc->enum_images));
	if (XR_FAILED(r) || sc->enum_images == nullptr) {
		PE("shell_chrome_create: xrEnumerateSwapchainImages PFN missing\n");
		delete sc;
		return nullptr;
	}

	if (!init_shader_pipeline(sc)) {
		PE("shell_chrome_create: shader pipeline init failed\n");
		delete sc;
		return nullptr;
	}

	P("shell_chrome: ready (device=%p, context=%p, pill shader compiled)\n",
	  (void *)sc->device, (void *)sc->context);
	return sc;
}

extern "C" void
shell_chrome_destroy(struct shell_chrome *sc)
{
	if (sc == nullptr) {
		return;
	}
	for (auto &slot : sc->slots) {
		release_slot_resources(slot, sc);
	}
	sc->slots.clear();
	delete sc;
}

extern "C" bool
shell_chrome_on_client_connected(struct shell_chrome *sc,
                                 XrWorkspaceClientId id,
                                 float win_w_m,
                                 float win_h_m)
{
	if (sc == nullptr || id == XR_NULL_WORKSPACE_CLIENT_ID) {
		return false;
	}
	if (chrome_slot *existing = find_slot(sc, id); existing != nullptr) {
		// Already created. Fast-path: if the window size has not changed,
		// no IPC traffic — main.c calls this every tick during the lazy
		// retry loop, and an unconditional set_chrome_layout RPC would
		// stall the slot-anim's set_pose IPC calls during transitions.
		const float eps = 1e-4f;
		bool size_changed = (fabsf(existing->win_w_m - win_w_m) > eps) ||
		                    (fabsf(existing->win_h_m - win_h_m) > eps);
		if (size_changed) {
			existing->win_w_m = win_w_m;
			existing->win_h_m = win_h_m;
			push_layout(sc, *existing);
		}
		return true;
	}

	XrWorkspaceChromeSwapchainCreateInfoEXT cinfo = {XR_TYPE_WORKSPACE_CHROME_SWAPCHAIN_CREATE_INFO_EXT};
	cinfo.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB_VAL;
	cinfo.width = CHROME_TEX_W;
	cinfo.height = CHROME_TEX_H;
	cinfo.sampleCount = 1;
	cinfo.mipCount = 1;

	XrSwapchain swapchain = XR_NULL_HANDLE;
	XrResult r = sc->xr->create_chrome_swapchain(sc->xr->session, id, &cinfo, &swapchain);
	if (XR_FAILED(r)) {
		// Connect-time race: slot may not be bound yet (Phase 2.K lesson 3).
		// Caller retries on next tick.
		PE("shell_chrome: create_chrome_swapchain(client=%u) failed: %s — will retry\n",
		   id, xr_result_str(r));
		return false;
	}

	chrome_slot slot = {};
	slot.id = id;
	slot.swapchain = swapchain;
	slot.win_w_m = win_w_m;
	slot.win_h_m = win_h_m;
	slot.rendered_once = false;

	if (!init_slot_d3d11(sc, slot)) {
		(void)sc->xr->destroy_chrome_swapchain(swapchain);
		return false;
	}

	if (!acquire_render_release(sc, slot)) {
		release_slot_resources(slot, sc);
		return false;
	}
	slot.rendered_once = true;

	sc->slots.push_back(std::move(slot));
	push_layout(sc, sc->slots.back());

	P("shell_chrome: chrome ready for client %u (window %.3f×%.3f m, pill %.3f×%.3f m)\n",
	  id, win_w_m, win_h_m, win_w_m * PILL_W_FRAC, PILL_HEIGHT_M);
	return true;
}

extern "C" void
shell_chrome_on_client_disconnected(struct shell_chrome *sc, XrWorkspaceClientId id)
{
	if (sc == nullptr) {
		return;
	}
	for (auto it = sc->slots.begin(); it != sc->slots.end(); ++it) {
		if (it->id == id) {
			release_slot_resources(*it, sc);
			sc->slots.erase(it);
			P("shell_chrome: chrome released for client %u\n", id);
			return;
		}
	}
}

extern "C" void
shell_chrome_on_window_resized(struct shell_chrome *sc,
                               XrWorkspaceClientId id,
                               float win_w_m,
                               float win_h_m)
{
	if (sc == nullptr) {
		return;
	}
	chrome_slot *slot = find_slot(sc, id);
	if (slot == nullptr) {
		return;
	}
	slot->win_w_m = win_w_m;
	slot->win_h_m = win_h_m;
	push_layout(sc, *slot);
}

extern "C" bool
shell_chrome_has(struct shell_chrome *sc, XrWorkspaceClientId id)
{
	if (sc == nullptr || id == XR_NULL_WORKSPACE_CLIENT_ID) {
		return false;
	}
	return find_slot(sc, id) != nullptr;
}
