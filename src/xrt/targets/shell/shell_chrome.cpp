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

// stb_image for PNG decode of per-app icons. The shell already includes
// stb headers via the launcher path — re-use here, header-only impl is
// disabled (defined elsewhere). If stb_image isn't already linked, fall
// back to the alternative decoder below (TODO).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_ONLY_PNG
#include "stb_image.h"

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

// Forward decls for helpers defined in the lower anonymous namespace —
// shell_chrome_on_window_resized (defined as extern "C" between the two
// namespaces) needs to call them.
struct chrome_slot;
uint64_t shell_chrome_now_ns();
void seed_fade(chrome_slot &slot, float target, uint64_t duration_ns);

// Pill geometry constants — mirror the runtime's in-process pill so the
// controller-rendered pill lands at the same place + size as the existing
// in-runtime chrome. Once C3.C adds full visual parity these stay; C5
// deletes the runtime-side mirror.
constexpr float PILL_W_FRAC      = 0.75f;   // pill width as a fraction of window width
constexpr float PILL_HEIGHT_M    = 0.008f;  // 8 mm tall — UI_TITLE_BAR_H_M default
constexpr float PILL_GAP_FRAC    = 0.5f;    // gap above content = pill height * gap_frac
constexpr float UI_BTN_W_M       = 0.008f;  // per-button slot width — matches runtime UI_BTN_W_M
constexpr float DOT_SIZE_M       = 0.001f;  // grip-dot diameter (1 mm)
constexpr float DOT_GAP_M        = 0.001f;  // grip-dot spacing (1 mm)
constexpr float BTN_INSET_FRAC   = 0.18f;   // visible-circle inset within button slot
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

	// C3.C-4: hover-fade state. `alpha` is the currently-rendered global
	// alpha multiplier baked into the chrome image (1 = visible, 0 = hidden).
	// On hover toggle the shell seeds a tween toward `target_alpha` over
	// `fade_duration_ns`; shell_chrome_tick lerps + re-renders until alpha
	// matches target, then idles. Eases via 1-(1-t)^3 (ease-out cubic) — same
	// curve as the runtime's slot_chrome_fade_tick that this replaces.
	float    alpha;
	float    target_alpha;
	float    fade_start_alpha;
	uint64_t fade_start_ns;
	uint64_t fade_duration_ns;

	// Phase 2.C C5 follow-up: hide-during-resize. WINDOW_POSE_CHANGED
	// can fire at ~60 Hz while the user drags an edge handle; re-rendering
	// the chrome image on every event is laggy + wastes IPC. Instead we
	// fade the pill out fast on the first event of a burst, defer the
	// (cheap) chrome image re-render until the burst settles, then fade
	// back in. resize_pending_until_ns is the deadline by which "no more
	// pose-change events for X ms" qualifies as resize-done; reset on
	// every event.
	uint64_t resize_pending_until_ns; // 0 = not in a resize burst

	// Phase 2.C C3.C-3a: per-app icon. Loaded from the registered_app's
	// icon_path PNG via stb_image and uploaded into a D3D11 texture +
	// SRV at chrome-create time. NULL if the app has no resolvable
	// icon (test apps without sidecar manifests). The pill shader
	// samples icon_srv at register t0 and skips the icon draw when
	// has_icon = 0 in the cbuffer.
	ComPtr<ID3D11Texture2D>        icon_texture;
	ComPtr<ID3D11ShaderResourceView> icon_srv;
};

} // namespace

// Constant buffer for the rounded-pill shader. Layout matches the HLSL cbuffer
// at register b0. Sized to a multiple of 16 bytes for D3D11.
//
// All sizes/positions are in PILL-SPACE METERS so the SDFs stay correct
// regardless of how the chrome image gets stretched onto the pill quad
// at composite time.
struct PillCB
{
	// Register 0: pill geometry
	float pill_size_m[2];     // pill width/height in METERS
	float corner_radius_m;    // pill corner radius in METERS (full pill = pill_h_m * 0.5)
	float btn_inset_frac;     // button visible-circle inset, 0.18 in the runtime

	// Register 1: button + grip-dot geometry + global fade alpha
	float btn_width_m;        // per-button slot width (UI_BTN_W_M = 0.008)
	float dot_size_m;         // grip-dot diameter (0.001)
	float dot_gap_m;          // grip-dot spacing (0.001)
	float fade_alpha;         // C3.C-4: global alpha multiplier (0..1) — hover-fade

	// Register 2-5: colors (alpha = base opacity). Pill bg = frosted blue,
	// close = red, btn = gray, dot = light gray. Hover-driven brightness
	// modulation can be folded in later — for now the fade_alpha above
	// drives the entire chrome's hover-in/out tween.
	float pill_color[4];
	float close_color[4];
	float btn_color[4];
	float dot_color[4];

	// Register 6: app icon control. has_icon = 1 enables the icon sample
	// at left of the pill; 0 disables (skips the texture sample so apps
	// without a resolvable icon don't render garbage). icon_size_m is
	// the visible icon's square half-extent in pill-space meters.
	float has_icon;
	float icon_size_m;
	float _pad1[2];
};
static_assert(sizeof(PillCB) % 16 == 0, "PillCB must be 16-byte aligned");
static_assert(sizeof(PillCB) == 112, "PillCB layout drift");

struct shell_chrome
{
	struct shell_openxr_state *xr;
	ID3D11Device              *device;          // not owned
	ID3D11DeviceContext       *context;         // not owned

	std::vector<chrome_slot> slots;

	// Phase 2.C C5 follow-up: cached most-recent hover transition target.
	// Lets the resize-end handler restore the right post-resize alpha
	// (fade in if currently hovered, stay hidden if not) without waiting
	// for the next POINTER_HOVER event — which won't fire if the cursor
	// is already over the slot at resize-end.
	XrWorkspaceClientId current_hover_id;

	// Resolved at create.
	PFN_xrEnumerateSwapchainImages enum_images;

	// Shader pipeline for the rounded-pill render. Compiled once at create.
	ComPtr<ID3D11VertexShader>   vs_pill;
	ComPtr<ID3D11PixelShader>    ps_pill;
	ComPtr<ID3D11Buffer>         cb_pill;
	ComPtr<ID3D11RasterizerState> rs_state;
	ComPtr<ID3D11BlendState>     bs_passthrough;  // overwrite RTV — controller owns the chrome image
	ComPtr<ID3D11DepthStencilState> dss_disabled;
	ComPtr<ID3D11SamplerState>   icon_sampler; // linear-clamp; sampled by the pill shader for per-app icons
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

// SDF-based pill chrome HLSL. Renders pill bg + 8-dot grip handle + 3
// circular buttons (close/min/max) into a single shader pass. All shape
// math is done in PILL-SPACE METERS (cbuffer-supplied) so corners stay
// circular regardless of image-to-quad stretch. Coverage is derived
// per-pixel via fwidth() so AA matches actual rasterization scale.
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
    float2 pill_size_m;
    float  corner_radius_m;
    float  btn_inset_frac;

    float  btn_width_m;
    float  dot_size_m;
    float  dot_gap_m;
    float  fade_alpha;

    float4 pill_color;
    float4 close_color;
    float4 btn_color;
    float4 dot_color;

    float  has_icon;
    float  icon_size_m;
    float2 _pad1;
};

Texture2D    icon_tex  : register(t0);
SamplerState icon_samp : register(s0);

// Signed distance to a rounded rectangle centered at origin with half-extents
// `b` (full width/height = 2b) and corner radius `r`.
float sdRoundedBox(float2 p, float2 b, float r)
{
    float2 d = abs(p) - (b - r);
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
}

float sdCircle(float2 p, float r)
{
    return length(p) - r;
}

// Standard "src over dst" Porter-Duff: composite `src` on top of `dst`.
// Both inputs use straight (non-premultiplied) alpha; output is straight too.
float4 over(float4 src, float4 dst)
{
    float a = src.a + dst.a * (1.0 - src.a);
    if (a <= 1e-6) return float4(0, 0, 0, 0);
    float3 c = (src.rgb * src.a + dst.rgb * dst.a * (1.0 - src.a)) / a;
    return float4(c, a);
}

// Convert an SDF distance into a coverage value (1 inside, 0 outside, smooth
// 1-pixel transition at the boundary). `aa` is the derivative-based pixel
// width measured in the same units as `dist`.
float cov(float dist, float aa)
{
    return saturate(0.5 - dist / max(aa, 1e-6));
}

float4 PS(VSOut input) : SV_Target
{
    float2 p = input.uv * pill_size_m;          // pill-space meters
    float2 center = pill_size_m * 0.5;

    // Per-shape SDFs (all in pill-space meters).
    float pill_dist = sdRoundedBox(p - center, pill_size_m * 0.5, corner_radius_m);

    // Buttons: 3 circles inset within their UI_BTN_W_M slot at the right edge.
    // Visible radius = (slot/2) * (1 - 2*inset).
    float btn_r_m = (btn_width_m * 0.5) * (1.0 - 2.0 * btn_inset_frac);
    float btn_y   = pill_size_m.y * 0.5;
    float close_dist = sdCircle(p - float2(pill_size_m.x - btn_width_m * 0.5, btn_y), btn_r_m);
    float min_dist   = sdCircle(p - float2(pill_size_m.x - btn_width_m * 1.5, btn_y), btn_r_m);
    float max_dist   = sdCircle(p - float2(pill_size_m.x - btn_width_m * 2.5, btn_y), btn_r_m);

    // Grip dots: 4×2 grid centered in the pill, dots are circles of dot_size/2.
    float dot_r_m = dot_size_m * 0.5;
    float grid_w  = 4.0 * dot_size_m + 3.0 * dot_gap_m;
    float grid_h  = 2.0 * dot_size_m + 1.0 * dot_gap_m;
    float2 grid_origin = center - float2(grid_w * 0.5, grid_h * 0.5);
    float dot_dist = 1e6;
    [unroll] for (int gr = 0; gr < 2; gr++) {
        [unroll] for (int gc = 0; gc < 4; gc++) {
            float2 dc = grid_origin + float2(
                (float)gc * (dot_size_m + dot_gap_m) + dot_size_m * 0.5,
                (float)gr * (dot_size_m + dot_gap_m) + dot_size_m * 0.5);
            dot_dist = min(dot_dist, sdCircle(p - dc, dot_r_m));
        }
    }

    // Per-shape AA scale — fwidth on each so each shape's edge gets 1-pixel
    // smoothing at its own scale (small shapes share the same image-space
    // pixel grid so they all give the same fwidth value, but keep the calls
    // explicit for clarity).
    float aa_pill  = fwidth(pill_dist);
    float aa_btn   = fwidth(close_dist);
    float aa_dot   = fwidth(dot_dist);

    // Compose back-to-front: pill bg, then icon, then dots, then buttons.
    float4 result = float4(0, 0, 0, 0);
    result = over(float4(pill_color.rgb,  pill_color.a  * cov(pill_dist,  aa_pill)), result);

    // App icon at the left of the pill, mirroring the close button at the
    // right. Sampled from icon_tex (loaded by the shell from the
    // registered_app's icon_path PNG) when has_icon = 1. Rounded-square
    // mask (corner radius 25% of icon half-extent) keeps the icon from
    // looking like a pixelated square against the glassy pill — matches
    // the soft folder-icon styling in the concept art.
    if (has_icon > 0.5) {
        float2 icon_center = float2(btn_width_m * 0.5, pill_size_m.y * 0.5);
        float2 icon_pos = p - icon_center;
        float icon_corner_r = icon_size_m * 0.40;
        float2 icon_d = abs(icon_pos) - (icon_size_m - icon_corner_r);
        float icon_dist = length(max(icon_d, 0.0)) +
                          min(max(icon_d.x, icon_d.y), 0.0) - icon_corner_r;
        if (icon_dist < 0.0) {
            // Map [-icon_size_m, +icon_size_m] → [0, 1]. Both pill-space
            // (p.y) and stb_image rows are top-down, so no y-flip needed.
            float2 icon_uv = float2(
                (icon_pos.x + icon_size_m) / (2.0 * icon_size_m),
                (icon_pos.y + icon_size_m) / (2.0 * icon_size_m));
            float4 icon_sample = icon_tex.Sample(icon_samp, icon_uv);
            float icon_cov = saturate(0.5 - icon_dist / max(fwidth(icon_dist), 1e-6));
            result = over(float4(icon_sample.rgb, icon_sample.a * icon_cov), result);
        }
    }

    // Pill edge highlight — thin bright rim along the perimeter, falls off
    // a few pixels each way. Reads as the "frosted glass refraction" cue
    // in the concept art (bright outer edge, soft body).
    float pill_edge = abs(pill_dist);
    float pill_edge_aa = max(fwidth(pill_dist), 1e-6);
    float pill_edge_glow = saturate(1.0 - pill_edge / (pill_edge_aa * 2.0));
    result.rgb = lerp(result.rgb, float3(1.0, 1.0, 1.0), pill_edge_glow * 0.35 * cov(pill_dist - aa_pill, aa_pill));

    result = over(float4(dot_color.rgb,   dot_color.a   * cov(dot_dist,   aa_dot)),  result);
    result = over(float4(btn_color.rgb,   btn_color.a   * cov(max_dist,   aa_btn)),  result);
    result = over(float4(btn_color.rgb,   btn_color.a   * cov(min_dist,   aa_btn)),  result);
    result = over(float4(close_color.rgb, close_color.a * cov(close_dist, aa_btn)),  result);

    // Procedural button glyphs (placeholder until DirectWrite atlas lands):
    //   close   = × (two diagonals)
    //   min     = − (horizontal bar)
    //   max     = □ (hollow square outline)
    // Rasterized in pill-space via SDF, masked to the visible-button circle
    // so they don't bleed onto the pill bg between buttons.
    float btn_y2 = pill_size_m.y * 0.5;
    float glyph_w_m = btn_r_m * 0.085; // stroke half-width in pill-space meters
    float glyph_size_m = btn_r_m * 0.55; // half-extent of the glyph bounding box
    float4 glyph_col = float4(0.97, 0.98, 1.0, 0.95); // bright white tint

    // Close (×): two diagonal capsules, length = glyph_size, half-width = glyph_w_m
    float2 cp = p - float2(pill_size_m.x - btn_width_m * 0.5, btn_y2);
    float close_diag1 = abs(cp.x + cp.y) * 0.7071 - glyph_w_m;
    float close_diag2 = abs(cp.x - cp.y) * 0.7071 - glyph_w_m;
    // Limit each diagonal's length by clipping outside the button-radius circle
    float close_mask = sdCircle(cp, glyph_size_m);
    float close_glyph = max(min(close_diag1, close_diag2), close_mask);
    result = over(float4(glyph_col.rgb, glyph_col.a * cov(close_glyph, fwidth(close_glyph))), result);

    // Minimize (−): horizontal capsule centered on the button
    float2 mp = p - float2(pill_size_m.x - btn_width_m * 1.5, btn_y2);
    float2 min_d = abs(mp) - float2(glyph_size_m, glyph_w_m);
    float min_glyph = length(max(min_d, 0.0)) + min(max(min_d.x, min_d.y), 0.0);
    result = over(float4(glyph_col.rgb, glyph_col.a * cov(min_glyph, fwidth(min_glyph))), result);

    // Maximize (□): hollow square outline (rounded rect minus inner rect)
    float2 xp = p - float2(pill_size_m.x - btn_width_m * 2.5, btn_y2);
    float2 max_outer_d = abs(xp) - float2(glyph_size_m, glyph_size_m);
    float max_outer = length(max(max_outer_d, 0.0)) + min(max(max_outer_d.x, max_outer_d.y), 0.0);
    float max_inner = max_outer + glyph_w_m * 1.4; // ring thickness
    float max_outline = max(max_outer, -max_inner);
    result = over(float4(glyph_col.rgb, glyph_col.a * cov(max_outline, fwidth(max_outline))), result);

    // C3.C-4 hover-fade: global alpha multiplier on the final RGBA.
    return float4(result.rgb, result.a * saturate(fade_alpha));
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

	// Linear-clamp sampler for app icons. Linear filter keeps the icon
	// crisp at varying display scales; clamp address mode prevents tiling
	// artifacts when the shader's UV math drifts off the [0,1] range.
	D3D11_SAMPLER_DESC ssd = {};
	ssd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	ssd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	ssd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	ssd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	ssd.MaxLOD = D3D11_FLOAT32_MAX;
	hr = sc->device->CreateSamplerState(&ssd, sc->icon_sampler.GetAddressOf());
	if (FAILED(hr)) { PE("shell_chrome: CreateSamplerState(icon) failed: 0x%08lx\n", hr); return false; }

	return true;
}

// Load a PNG from disk and create an immutable D3D11 texture + SRV. Returns
// true on success; on failure logs and leaves the slot's icon_* fields
// unmodified. Format is forced to R8G8B8A8_UNORM_SRGB so the icon is
// gamma-correct when sampled by the chrome shader.
bool
load_icon_png(shell_chrome *sc, chrome_slot &slot, const char *png_path)
{
	if (sc == nullptr || png_path == nullptr || png_path[0] == '\0') {
		return false;
	}
	int w = 0, h = 0, n = 0;
	stbi_uc *pixels = stbi_load(png_path, &w, &h, &n, 4);
	if (pixels == nullptr) {
		PE("shell_chrome: stbi_load failed for '%s': %s\n", png_path, stbi_failure_reason());
		return false;
	}
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = (UINT)w;
	td.Height = (UINT)h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_IMMUTABLE;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA sd = {};
	sd.pSysMem = pixels;
	sd.SysMemPitch = (UINT)(w * 4);
	HRESULT hr = sc->device->CreateTexture2D(&td, &sd, slot.icon_texture.GetAddressOf());
	stbi_image_free(pixels);
	if (FAILED(hr)) {
		PE("shell_chrome: CreateTexture2D(icon) failed for '%s': 0x%08lx\n", png_path, hr);
		return false;
	}
	hr = sc->device->CreateShaderResourceView(slot.icon_texture.Get(), nullptr, slot.icon_srv.GetAddressOf());
	if (FAILED(hr)) {
		PE("shell_chrome: CreateShaderResourceView(icon) failed for '%s': 0x%08lx\n", png_path, hr);
		slot.icon_texture.Reset();
		return false;
	}
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

	// Match the in-runtime chrome geometry + colors so C3.C-2 lands at
	// visual parity. Hover state and per-button color modulation arrive
	// in C3.C-4 — for now buttons are static (no hover).
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
	cb->btn_inset_frac = BTN_INSET_FRAC;

	cb->btn_width_m = UI_BTN_W_M;
	cb->dot_size_m = DOT_SIZE_M;
	cb->dot_gap_m  = DOT_GAP_M;
	// C3.C-4 hover-fade: bake the slot's current fade alpha into this
	// chrome render. The shader applies it as a global multiplier on the
	// final pixel alpha — fade_alpha=0 hides the chrome entirely (idle
	// steady state); fade_alpha=1 shows it fully (cursor over chrome).
	cb->fade_alpha = slot.alpha;

	// Glassy / frosted pill bg — light cool tint, mostly transparent so
	// content shows through. Edge highlight (in the shader) gives the
	// "frosted glass refraction" cue at the perimeter. Matches the
	// concept-art direction (semi-transparent capsule, brighter rim).
	cb->pill_color[0] = 0.78f; cb->pill_color[1] = 0.84f;
	cb->pill_color[2] = 0.92f; cb->pill_color[3] = 0.22f;

	// Close button — coral red, semi-transparent so the pill bg shows
	// through and the white × glyph reads cleanly on top.
	cb->close_color[0] = 0.92f; cb->close_color[1] = 0.32f;
	cb->close_color[2] = 0.36f; cb->close_color[3] = 0.65f;

	// Min/max buttons — light cool gray, semi-transparent.
	cb->btn_color[0] = 0.78f; cb->btn_color[1] = 0.82f;
	cb->btn_color[2] = 0.88f; cb->btn_color[3] = 0.45f;

	// Grip dots — bright white tint, low opacity so the cluster reads as
	// a subtle drag affordance not a bold visual block.
	cb->dot_color[0] = 0.95f; cb->dot_color[1] = 0.97f;
	cb->dot_color[2] = 1.00f; cb->dot_color[3] = 0.55f;

	// App icon: visible at the left of the pill. Square half-extent ≈
	// 70% of pill half-height (mirroring the visible button radius after
	// the BTN_INSET_FRAC inset). Skipped when no icon was resolvable.
	cb->has_icon = (slot.icon_srv != nullptr) ? 1.0f : 0.0f;
	cb->icon_size_m = pill_h_m * 0.5f * (1.0f - 2.0f * BTN_INSET_FRAC);
	cb->_pad1[0] = 0.0f;
	cb->_pad1[1] = 0.0f;

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

	// Bind the icon SRV at register t0 (pill shader samples it iff
	// has_icon = 1) and a linear-clamp sampler at s0. When has_icon = 0
	// we still bind a NULL SRV so the previous slot's icon doesn't leak
	// across draws.
	ID3D11ShaderResourceView *icon_srv = slot.icon_srv ? slot.icon_srv.Get() : nullptr;
	sc->context->PSSetShaderResources(0, 1, &icon_srv);
	sc->context->PSSetSamplers(0, 1, sc->icon_sampler.GetAddressOf());

	sc->context->Draw(4, 0);

	// Flush so the GPU work is submitted before xrReleaseSwapchainImage
	// releases the keyed mutex — the runtime's chrome composite (on a
	// different D3D11 device) reads via the shared NT handle and would
	// otherwise sample stale or uninitialized memory.
	sc->context->Flush();
}

// Build the chrome layout struct: pose-in-client (above the window content),
// size (75% width × pill height), and the chrome-UV-space hit regions that
// match the pill rendering — close/min/max each take a UI_BTN_W_M-wide
// vertical slot at the right edge, grip is the centered 4×2 dot grid.
//
// Chrome-UV space: (0,0) = top-left of chrome image, (1,1) = bottom-right.
// The runtime ray-casts the chrome quad and looks up the first region whose
// UV bounds contain the hit; the matched region's id is echoed back as
// chromeRegionId on POINTER / POINTER_MOTION events.
void
push_layout(shell_chrome *sc, chrome_slot &slot)
{
	const float pill_w_m = slot.win_w_m * PILL_W_FRAC;
	const float pill_h_m = PILL_HEIGHT_M;
	const float gap_m    = pill_h_m * PILL_GAP_FRAC;
	// spec_version 8: with anchorToWindowTopEdge = XR_TRUE, position.y is
	// interpreted as the offset ABOVE the window's TOP edge — not from
	// window center. The runtime auto-recomputes effective position each
	// frame from the current window height, so the chrome stays glued to
	// the top edge during a resize without the controller having to
	// re-push layout (which lagged one frame behind the runtime). The
	// offset (gap + pill_h/2) is invariant to win_h_m.
	const float pill_offset_above_top = gap_m + (pill_h_m * 0.5f);

	// Per-button slot in chrome-UV: each slot is UI_BTN_W_M wide. Mirrors
	// the shader's pill-space layout (close on right, then min, then max).
	const float btn_uw = UI_BTN_W_M / pill_w_m;

	// Grip rect in chrome-UV: 4 dots × 1 mm + 3 gaps × 1 mm = 7 mm wide,
	// 2 dots × 1 mm + 1 gap × 1 mm = 3 mm tall, centered in the pill.
	const float grip_w_m = 4.0f * DOT_SIZE_M + 3.0f * DOT_GAP_M;
	const float grip_h_m = 2.0f * DOT_SIZE_M + 1.0f * DOT_GAP_M;
	const float grip_uw  = grip_w_m / pill_w_m;
	const float grip_uh  = grip_h_m / pill_h_m;

	XrWorkspaceChromeHitRegionEXT regions[4];
	// Close (rightmost slot)
	regions[0].id = SHELL_CHROME_REGION_CLOSE;
	regions[0].bounds.offset.x = 1.0f - btn_uw;
	regions[0].bounds.offset.y = 0.0f;
	regions[0].bounds.extent.width = btn_uw;
	regions[0].bounds.extent.height = 1.0f;
	// Minimize (one slot left of close)
	regions[1].id = SHELL_CHROME_REGION_MIN;
	regions[1].bounds.offset.x = 1.0f - 2.0f * btn_uw;
	regions[1].bounds.offset.y = 0.0f;
	regions[1].bounds.extent.width = btn_uw;
	regions[1].bounds.extent.height = 1.0f;
	// Maximize (two slots left of close)
	regions[2].id = SHELL_CHROME_REGION_MAX;
	regions[2].bounds.offset.x = 1.0f - 3.0f * btn_uw;
	regions[2].bounds.offset.y = 0.0f;
	regions[2].bounds.extent.width = btn_uw;
	regions[2].bounds.extent.height = 1.0f;
	// Grip (centered)
	regions[3].id = SHELL_CHROME_REGION_GRIP;
	regions[3].bounds.offset.x = 0.5f - grip_uw * 0.5f;
	regions[3].bounds.offset.y = 0.5f - grip_uh * 0.5f;
	regions[3].bounds.extent.width = grip_uw;
	regions[3].bounds.extent.height = grip_uh;

	XrWorkspaceChromeLayoutEXT layout = {XR_TYPE_WORKSPACE_CHROME_LAYOUT_EXT};
	layout.poseInClient.orientation.x = 0.0f;
	layout.poseInClient.orientation.y = 0.0f;
	layout.poseInClient.orientation.z = 0.0f;
	layout.poseInClient.orientation.w = 1.0f;
	layout.poseInClient.position.x = 0.0f;
	layout.poseInClient.position.y = pill_offset_above_top;
	layout.poseInClient.position.z = 0.0f;
	// sizeMeters.width is ignored when widthAsFractionOfWindow > 0 (the
	// runtime auto-scales to win_w * fraction every frame). Set both
	// anyway in case some path falls back to absolute width.
	layout.sizeMeters.width = pill_w_m;
	layout.sizeMeters.height = pill_h_m;
	layout.followsWindowOrient = XR_TRUE;
	layout.hitRegionCount = 4;
	layout.hitRegions = regions;
	layout.depthBiasMeters = 0.0f; // 0 = use runtime default
	layout.anchorToWindowTopEdge = XR_TRUE;
	layout.widthAsFractionOfWindow = PILL_W_FRAC;

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
                                 float win_h_m,
                                 const char *icon_png_path)
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
	// C3.C-4: chrome is fully visible at connect. Once main.c starts
	// feeding hover events via shell_chrome_set_hover, the standard
	// fade-out-when-not-hovered / fade-in-on-hover behavior kicks in.
	slot.alpha = 1.0f;
	slot.target_alpha = 1.0f;
	slot.fade_start_alpha = 1.0f;
	slot.fade_start_ns = 0;
	slot.fade_duration_ns = 0;

	if (!init_slot_d3d11(sc, slot)) {
		(void)sc->xr->destroy_chrome_swapchain(swapchain);
		return false;
	}

	// Phase 2.C C3.C-3a: load app icon BEFORE first render so it's part
	// of the initial pill image. Best-effort — apps without an icon path
	// (test apps, unregistered apps) just render iconless pills.
	if (icon_png_path != nullptr && icon_png_path[0] != '\0') {
		(void)load_icon_png(sc, slot, icon_png_path);
	}

	if (!acquire_render_release(sc, slot)) {
		release_slot_resources(slot, sc);
		return false;
	}
	slot.rendered_once = true;

	sc->slots.push_back(std::move(slot));
	push_layout(sc, sc->slots.back());

	P("shell_chrome: chrome ready for client %u (window %.3f×%.3f m, pill %.3f×%.3f m, icon=%s)\n",
	  id, win_w_m, win_h_m, win_w_m * PILL_W_FRAC, PILL_HEIGHT_M,
	  (icon_png_path && icon_png_path[0]) ? icon_png_path : "(none)");
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
	const float kEps = 1e-5f;
	bool size_changed =
	    fabsf(slot->win_w_m - win_w_m) > kEps ||
	    fabsf(slot->win_h_m - win_h_m) > kEps;
	if (!size_changed) {
		return;
	}
	slot->win_w_m = win_w_m;
	slot->win_h_m = win_h_m;

	// spec_version 8 win: the layout was pushed once at chrome-create with
	// anchorToWindowTopEdge=true and widthAsFractionOfWindow=PILL_W_FRAC,
	// so the runtime auto-tracks the window edge every frame using the
	// CURRENT window dims. NO push_layout is needed here — that call was
	// the source of the visible resize lag (controller's pose was based
	// on stale win_h, runtime composited the result one frame behind).
	// The only deferred work is the chrome IMAGE re-render: the pill
	// shape (rounded SDF + button glyphs) is rasterized in pill-space
	// meters, and a width change scales pill_w_m which scales the SDF.
	// We let that re-render happen at burst end so the rounded ends and
	// dot/button positions snap to crisp final geometry.
	//
	// 100 ms debounce balances "snap quickly after release" (≤ 1 perceived
	// frame at 60 Hz feels instant) against re-rendering during a long
	// drag. Shorter would re-render more often during resize; longer
	// leaves the pill visibly distorted for longer after release.
	constexpr uint64_t RESIZE_DEBOUNCE_NS = 100ULL * 1000000ULL;
	slot->resize_pending_until_ns = shell_chrome_now_ns() + RESIZE_DEBOUNCE_NS;
}

extern "C" bool
shell_chrome_has(struct shell_chrome *sc, XrWorkspaceClientId id)
{
	if (sc == nullptr || id == XR_NULL_WORKSPACE_CLIENT_ID) {
		return false;
	}
	return find_slot(sc, id) != nullptr;
}

namespace {

// C3.C-4: chrome fade durations — match the runtime's Phase 2.K constants
// so the controller-side fade feels identical to the runtime fallback.
// Hover-IN is faster than hover-OUT (visible cue should land quickly when
// the user moves over a window; fade-out lingers so the chrome doesn't
// pop out from under the cursor on quick passes).
constexpr uint64_t FADE_IN_NS  = 150ULL * 1000000ULL;
constexpr uint64_t FADE_OUT_NS = 300ULL * 1000000ULL;

uint64_t
shell_chrome_now_ns()
{
#ifdef _WIN32
	LARGE_INTEGER c, f;
	QueryPerformanceCounter(&c);
	QueryPerformanceFrequency(&f);
	return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
	struct timespec ts;
	timespec_get(&ts, TIME_UTC);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

// Seed a fade toward `target` over `duration_ns`, picking up from the
// slot's current alpha so consecutive hover toggles don't snap.
void
seed_fade(chrome_slot &slot, float target, uint64_t duration_ns)
{
	if (fabsf(slot.target_alpha - target) < 1e-4f && slot.fade_duration_ns != 0) {
		return; // already heading there
	}
	slot.fade_start_alpha = slot.alpha;
	slot.target_alpha = target;
	slot.fade_start_ns = shell_chrome_now_ns();
	slot.fade_duration_ns = duration_ns;
}

// Returns true if the slot's alpha changed this tick (caller should re-render).
bool
tick_fade(chrome_slot &slot)
{
	if (slot.fade_duration_ns == 0) {
		return false; // no animation in flight
	}
	uint64_t now = shell_chrome_now_ns();
	uint64_t elapsed = (now >= slot.fade_start_ns) ? now - slot.fade_start_ns : 0;
	if (elapsed >= slot.fade_duration_ns) {
		slot.alpha = slot.target_alpha;
		slot.fade_duration_ns = 0;
		return true;
	}
	float t = (float)((double)elapsed / (double)slot.fade_duration_ns);
	float f = 1.0f - t;
	float eased = 1.0f - f * f * f; // ease-out cubic
	slot.alpha = slot.fade_start_alpha + (slot.target_alpha - slot.fade_start_alpha) * eased;
	return true;
}

// Re-render just the chrome image without re-pushing the layout. Used by
// the fade tick to update the alpha bake; geometry is unchanged.
void
rerender_only(shell_chrome *sc, chrome_slot &slot)
{
	(void)acquire_render_release(sc, slot);
}

} // namespace

extern "C" void
shell_chrome_set_hover(struct shell_chrome *sc, XrWorkspaceClientId hover_id)
{
	if (sc == nullptr) {
		return;
	}
	sc->current_hover_id = hover_id;
	for (auto &s : sc->slots) {
		float target = (s.id == hover_id) ? 1.0f : 0.0f;
		uint64_t dur = (s.id == hover_id) ? FADE_IN_NS : FADE_OUT_NS;
		seed_fade(s, target, dur);
	}
}

extern "C" void
shell_chrome_tick(struct shell_chrome *sc)
{
	if (sc == nullptr) {
		return;
	}
	uint64_t now = shell_chrome_now_ns();
	for (auto &s : sc->slots) {
		// Phase 2.C C5 follow-up: detect resize-end. resize_pending_until_ns
		// is set on every WINDOW_POSE_CHANGED to (now + 250 ms); when the
		// burst stops, re-render the image once at the final dims so the
		// rounded ends + grip dots + buttons + icon snap to crisp
		// geometry (during the burst they were sampled from an image
		// rendered at the burst-start aspect). Layout was kept in sync
		// per event; only the image bake was deferred.
		if (s.resize_pending_until_ns != 0 && now >= s.resize_pending_until_ns) {
			s.resize_pending_until_ns = 0;
			rerender_only(sc, s); // bake new pill geometry at final aspect
		}
		if (tick_fade(s)) {
			rerender_only(sc, s);
		}
	}
}

extern "C" bool
shell_chrome_is_animating(struct shell_chrome *sc)
{
	if (sc == nullptr) {
		return false;
	}
	for (auto &s : sc->slots) {
		if (s.fade_duration_ns != 0) {
			return true;
		}
		// Resize-pending counts as "animating" so the main loop keeps
		// the 16 ms tick cadence and shell_chrome_tick fires in time
		// to detect the debounce expiry.
		if (s.resize_pending_until_ns != 0) {
			return true;
		}
	}
	return false;
}

extern "C" bool
shell_chrome_has_any(struct shell_chrome *sc)
{
	return sc != nullptr && !sc->slots.empty();
}
