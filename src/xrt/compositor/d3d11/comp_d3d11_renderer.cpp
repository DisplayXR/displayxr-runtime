// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 renderer implementation for layer compositing.
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#include "comp_d3d11_renderer.h"
#include "comp_d3d11_compositor.h"
#include "comp_d3d11_swapchain.h"

#include "util/comp_layer_accum.h"
#include "util/u_logging.h"
#include "math/m_api.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

/*!
 * Shader constant buffer layout.
 */
struct LayerConstants
{
	float mvp[16];          // Model-view-projection matrix
	float post_transform[4]; // xy = offset, zw = scale
	float color_scale[4];    // Color multiplier
	float color_bias[4];     // Color offset
};

/*!
 * D3D11 renderer structure.
 */
struct comp_d3d11_renderer
{
	//! Parent compositor.
	struct comp_d3d11_compositor *c;

	//! Side-by-side atlas texture.
	ID3D11Texture2D *atlas_texture;

	//! SRV for atlas texture (for weaver input).
	ID3D11ShaderResourceView *atlas_srv;

	//! RTV for atlas texture (for rendering).
	ID3D11RenderTargetView *atlas_rtv;

	//! Depth texture.
	ID3D11Texture2D *depth_texture;

	//! DSV for depth texture.
	ID3D11DepthStencilView *depth_dsv;

	//! Vertex shader for projection layers.
	ID3D11VertexShader *projection_vs;

	//! Pixel shader for projection layers.
	ID3D11PixelShader *projection_ps;

	//! Vertex shader for quad layers.
	ID3D11VertexShader *quad_vs;

	//! Pixel shader for quad layers.
	ID3D11PixelShader *quad_ps;

	//! Constant buffer for shader parameters.
	ID3D11Buffer *constant_buffer;

	//! Linear sampler.
	ID3D11SamplerState *sampler_linear;

	//! Point sampler.
	ID3D11SamplerState *sampler_point;

	//! Blend state for alpha blending.
	ID3D11BlendState *blend_alpha;

	//! Blend state for premultiplied alpha.
	ID3D11BlendState *blend_premul;

	//! Blend state for opaque.
	ID3D11BlendState *blend_opaque;

	//! Rasterizer state.
	ID3D11RasterizerState *rasterizer_state;

	//! Depth stencil state.
	ID3D11DepthStencilState *depth_stencil_state;

	//! View dimensions (per-eye).
	uint32_t view_width;
	uint32_t view_height;

	//! Tile layout for atlas (e.g. 2x1 for stereo, 2x2 for quad).
	uint32_t tile_columns;
	uint32_t tile_rows;

	//! Texture height (may be > view_height to accommodate mono/2D mode).
	//! The atlas texture is tile_columns*view_width x texture_height.
	uint32_t texture_height;

	//! When true, view dims are fixed at legacy compromise scale and
	//! set_tile_layout must not recompute them.
	bool legacy_app_tile_scaling;
};

// Access compositor internals
extern "C" {
struct comp_d3d11_compositor_internals
{
	struct xrt_compositor_native base;
	struct xrt_device *xdev;
	ID3D11Device *device;
	ID3D11DeviceContext *context;
	IDXGIFactory4 *dxgi_factory;
};
}

static inline struct comp_d3d11_compositor_internals *
get_internals(struct comp_d3d11_compositor *c)
{
	return reinterpret_cast<struct comp_d3d11_compositor_internals *>(c);
}

// Embedded HLSL shader source
static const char *projection_vs_source = R"(
cbuffer LayerCB : register(b0)
{
    float4x4 mvp;
    float4 post_transform;
    float4 color_scale;
    float4 color_bias;
};

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

static const float2 quad_positions[4] = {
    float2(-1.0, -1.0),
    float2(-1.0,  1.0),
    float2( 1.0, -1.0),
    float2( 1.0,  1.0),
};

static const float2 quad_uvs[4] = {
    float2(0.0, 1.0),
    float2(0.0, 0.0),
    float2(1.0, 1.0),
    float2(1.0, 0.0),
};

VS_OUTPUT VSMain(uint vertex_id : SV_VertexID)
{
    VS_OUTPUT output;
    float2 pos = quad_positions[vertex_id];
    float2 uv = quad_uvs[vertex_id];
    output.position = mul(mvp, float4(pos, 0.0, 1.0));
    output.uv = uv * post_transform.zw + post_transform.xy;
    return output;
}
)";

static const char *projection_ps_source = R"(
cbuffer LayerCB : register(b0)
{
    float4x4 mvp;
    float4 post_transform;
    float4 color_scale;
    float4 color_bias;
};

Texture2D layer_tex : register(t0);
SamplerState layer_samp : register(s0);

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float4 color = layer_tex.Sample(layer_samp, input.uv);
    color = color * color_scale + color_bias;
    return color;
}
)";

// Quad layer vertex shader - positioned 3D quad
static const char *quad_vs_source = R"(
cbuffer LayerCB : register(b0)
{
    float4x4 mvp;
    float4 post_transform;
    float4 color_scale;
    float4 color_bias;
};

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Quad centered at origin, 1x1 size in local space
static const float2 quad_positions[4] = {
    float2(0.0, 0.0),   // Bottom-left
    float2(0.0, 1.0),   // Top-left
    float2(1.0, 0.0),   // Bottom-right
    float2(1.0, 1.0),   // Top-right
};

VS_OUTPUT VSMain(uint vertex_id : SV_VertexID)
{
    VS_OUTPUT output;

    float2 in_uv = quad_positions[vertex_id % 4];

    // Center the quad at origin
    float2 pos = in_uv - 0.5;

    // No Y flip: the Vulkan-style projection (negative a22) already inverts Y,
    // which maps OpenXR's +Y-up world to D3D11's screen correctly.
    // (Vulkan compositor would need Y flip because Vulkan NDC has Y-down,
    // but D3D11 NDC has Y-up, matching OpenGL convention.)

    // Transform position by MVP (which includes quad size scaling)
    output.position = mul(mvp, float4(pos, 0.0, 1.0));

    // Apply UV transform for sub-image
    output.uv = in_uv * post_transform.zw + post_transform.xy;

    return output;
}
)";

// Quad layer pixel shader
static const char *quad_ps_source = R"(
cbuffer LayerCB : register(b0)
{
    float4x4 mvp;
    float4 post_transform;
    float4 color_scale;
    float4 color_bias;
};

Texture2D layer_tex : register(t0);
SamplerState layer_samp : register(s0);

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float4 color = layer_tex.Sample(layer_samp, input.uv);
    color = color * color_scale + color_bias;
    return color;
}
)";

static xrt_result_t
compile_shader(ID3D11Device *device,
               const char *source,
               const char *entry,
               const char *target,
               ID3DBlob **out_blob)
{
	ID3DBlob *errors = nullptr;
	HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, entry, target, 0, 0, out_blob,
	                        &errors);
	if (FAILED(hr)) {
		if (errors != nullptr) {
			U_LOG_E("Shader compile error: %s", (char *)errors->GetBufferPointer());
			errors->Release();
		}
		return XRT_ERROR_D3D;
	}
	if (errors != nullptr) {
		errors->Release();
	}
	return XRT_SUCCESS;
}

static xrt_result_t
create_shaders(struct comp_d3d11_renderer *r)
{
	auto internals = get_internals(r->c);
	ID3DBlob *blob = nullptr;

	// Compile vertex shader
	xrt_result_t xret = compile_shader(internals->device, projection_vs_source, "VSMain", "vs_5_0", &blob);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to compile vertex shader");
		return xret;
	}

	HRESULT hr = internals->device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                                    &r->projection_vs);
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create vertex shader: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Compile pixel shader
	xret = compile_shader(internals->device, projection_ps_source, "PSMain", "ps_5_0", &blob);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to compile pixel shader");
		return xret;
	}

	hr = internals->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                           &r->projection_ps);
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create pixel shader: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Compile quad vertex shader
	xret = compile_shader(internals->device, quad_vs_source, "VSMain", "vs_5_0", &blob);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to compile quad vertex shader");
		return xret;
	}

	hr = internals->device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                            &r->quad_vs);
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create quad vertex shader: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Compile quad pixel shader
	xret = compile_shader(internals->device, quad_ps_source, "PSMain", "ps_5_0", &blob);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to compile quad pixel shader");
		return xret;
	}

	hr = internals->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                           &r->quad_ps);
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create quad pixel shader: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	return XRT_SUCCESS;
}

static xrt_result_t
create_resources(struct comp_d3d11_renderer *r)
{
	auto internals = get_internals(r->c);

	// Create atlas texture (tile_columns * view_width).
	// Height is texture_height which may be > view_height to accommodate
	// mono (2D) rendering at full window resolution.
	D3D11_TEXTURE2D_DESC texDesc = {};
	texDesc.Width = r->tile_columns * r->view_width;
	texDesc.Height = r->texture_height;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = 1;
	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	texDesc.SampleDesc.Count = 1;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

	HRESULT hr = internals->device->CreateTexture2D(&texDesc, nullptr, &r->atlas_texture);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create atlas texture: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	U_LOG_W("Created atlas texture: %ux%u (view=%ux%u, tiles=%ux%u, tex_h=%u)",
	        texDesc.Width, texDesc.Height, r->view_width, r->view_height,
	        r->tile_columns, r->tile_rows, r->texture_height);

	// Create SRV for atlas texture
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = texDesc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;

	hr = internals->device->CreateShaderResourceView(r->atlas_texture, &srvDesc, &r->atlas_srv);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create atlas SRV: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Create RTV for atlas texture
	hr = internals->device->CreateRenderTargetView(r->atlas_texture, nullptr, &r->atlas_rtv);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create atlas RTV: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Create depth texture
	texDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

	hr = internals->device->CreateTexture2D(&texDesc, nullptr, &r->depth_texture);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create depth texture: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Create DSV
	hr = internals->device->CreateDepthStencilView(r->depth_texture, nullptr, &r->depth_dsv);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create depth DSV: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Create constant buffer
	D3D11_BUFFER_DESC cbDesc = {};
	cbDesc.ByteWidth = sizeof(LayerConstants);
	cbDesc.Usage = D3D11_USAGE_DYNAMIC;
	cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	hr = internals->device->CreateBuffer(&cbDesc, nullptr, &r->constant_buffer);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create constant buffer: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Create linear sampler
	D3D11_SAMPLER_DESC sampDesc = {};
	sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;

	hr = internals->device->CreateSamplerState(&sampDesc, &r->sampler_linear);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create linear sampler: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Create point sampler
	sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;

	hr = internals->device->CreateSamplerState(&sampDesc, &r->sampler_point);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create point sampler: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Create blend states
	D3D11_BLEND_DESC blendDesc = {};
	blendDesc.RenderTarget[0].BlendEnable = TRUE;
	blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

	hr = internals->device->CreateBlendState(&blendDesc, &r->blend_alpha);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create alpha blend state: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Premultiplied alpha
	blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;

	hr = internals->device->CreateBlendState(&blendDesc, &r->blend_premul);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create premul blend state: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Opaque
	blendDesc.RenderTarget[0].BlendEnable = FALSE;

	hr = internals->device->CreateBlendState(&blendDesc, &r->blend_opaque);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create opaque blend state: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Create rasterizer state
	D3D11_RASTERIZER_DESC rasterDesc = {};
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.CullMode = D3D11_CULL_NONE;
	rasterDesc.FrontCounterClockwise = FALSE;
	rasterDesc.DepthClipEnable = TRUE;

	hr = internals->device->CreateRasterizerState(&rasterDesc, &r->rasterizer_state);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create rasterizer state: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Create depth stencil state
	D3D11_DEPTH_STENCIL_DESC dsDesc = {};
	dsDesc.DepthEnable = FALSE;
	dsDesc.StencilEnable = FALSE;

	hr = internals->device->CreateDepthStencilState(&dsDesc, &r->depth_stencil_state);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create depth stencil state: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	return XRT_SUCCESS;
}

static void
render_projection_layer(struct comp_d3d11_renderer *r,
                        struct comp_layer *layer,
                        uint32_t view_index,
                        struct xrt_vec3 *eye_position)
{
	auto internals = get_internals(r->c);
	(void)eye_position;

	// Get swapchain for this view
	struct xrt_swapchain *xsc = layer->sc_array[view_index];
	if (xsc == nullptr) {
		U_LOG_W("render_projection_layer: swapchain is null for view %u", view_index);
		return;
	}

	// Get the image index from the layer data
	struct xrt_layer_projection_view_data *view_data = &layer->data.proj.v[view_index];
	uint32_t image_index = view_data->sub.image_index;

	// Get the D3D11 swapchain's SRV for this image
	ID3D11ShaderResourceView *srv = static_cast<ID3D11ShaderResourceView *>(
	    comp_d3d11_swapchain_get_srv(xsc, image_index));
	if (srv == nullptr) {
		U_LOG_W("render_projection_layer: SRV is null for swapchain image %u", image_index);
		return;
	}

	// Set projection shaders explicitly (must be done per-draw because quad layer
	// rendering switches to quad shaders, and those persist to the next projection layer)
	internals->context->VSSetShader(r->projection_vs, nullptr, 0);
	internals->context->PSSetShader(r->projection_ps, nullptr, 0);

	// Update constant buffer
	LayerConstants constants = {};

	// Identity MVP (fullscreen quad)
	memset(constants.mvp, 0, sizeof(constants.mvp));
	constants.mvp[0] = 1.0f;
	constants.mvp[5] = 1.0f;
	constants.mvp[10] = 1.0f;
	constants.mvp[15] = 1.0f;

	// Use normalized rect for UV transform (view_data already obtained above)
	constants.post_transform[0] = view_data->sub.norm_rect.x;
	constants.post_transform[1] = view_data->sub.norm_rect.y;
	constants.post_transform[2] = view_data->sub.norm_rect.w;
	constants.post_transform[3] = view_data->sub.norm_rect.h;

	// Handle Y-flip (e.g. OpenGL textures have bottom-left origin)
	if (layer->data.flip_y) {
		constants.post_transform[1] += constants.post_transform[3];
		constants.post_transform[3] = -constants.post_transform[3];
	}

	// Color scale and bias
	// Default to identity (scale=1, bias=0) if not explicitly set via
	// XR_KHR_composition_layer_color_scale_bias extension
	bool has_color_scale_bias = (layer->data.flags & XRT_LAYER_COMPOSITION_COLOR_BIAS_SCALE) != 0;
	if (has_color_scale_bias) {
		constants.color_scale[0] = layer->data.color_scale.r;
		constants.color_scale[1] = layer->data.color_scale.g;
		constants.color_scale[2] = layer->data.color_scale.b;
		constants.color_scale[3] = layer->data.color_scale.a;
		constants.color_bias[0] = layer->data.color_bias.r;
		constants.color_bias[1] = layer->data.color_bias.g;
		constants.color_bias[2] = layer->data.color_bias.b;
		constants.color_bias[3] = layer->data.color_bias.a;
	} else {
		// Default: no color modification
		constants.color_scale[0] = 1.0f;
		constants.color_scale[1] = 1.0f;
		constants.color_scale[2] = 1.0f;
		constants.color_scale[3] = 1.0f;
		constants.color_bias[0] = 0.0f;
		constants.color_bias[1] = 0.0f;
		constants.color_bias[2] = 0.0f;
		constants.color_bias[3] = 0.0f;
	}

	// Map and update constant buffer
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = internals->context->Map(r->constant_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (SUCCEEDED(hr)) {
		memcpy(mapped.pData, &constants, sizeof(constants));
		internals->context->Unmap(r->constant_buffer, 0);
	}

	// Set shader resources
	internals->context->VSSetConstantBuffers(0, 1, &r->constant_buffer);
	internals->context->PSSetConstantBuffers(0, 1, &r->constant_buffer);
	internals->context->PSSetSamplers(0, 1, &r->sampler_linear);

	// Bind the swapchain texture as shader resource
	internals->context->PSSetShaderResources(0, 1, &srv);

	// Draw fullscreen quad (triangle strip, 4 vertices)
	internals->context->Draw(4, 0);

	// Unbind SRV to avoid resource hazards
	ID3D11ShaderResourceView *null_srv = nullptr;
	internals->context->PSSetShaderResources(0, 1, &null_srv);
}

static bool
is_layer_view_visible(const struct xrt_layer_data *data, uint32_t view_index)
{
	enum xrt_layer_eye_visibility visibility;

	switch (data->type) {
	case XRT_LAYER_QUAD: visibility = data->quad.visibility; break;
	case XRT_LAYER_CYLINDER: visibility = data->cylinder.visibility; break;
	case XRT_LAYER_EQUIRECT1: visibility = data->equirect1.visibility; break;
	case XRT_LAYER_EQUIRECT2: visibility = data->equirect2.visibility; break;
	case XRT_LAYER_CUBE: visibility = data->cube.visibility; break;
	default: return true; // Projection layers visible in both
	}

	switch (visibility) {
	case XRT_LAYER_EYE_VISIBILITY_NONE: return false;
	case XRT_LAYER_EYE_VISIBILITY_LEFT_BIT: return view_index == 0;
	case XRT_LAYER_EYE_VISIBILITY_RIGHT_BIT: return view_index == 1;
	case XRT_LAYER_EYE_VISIBILITY_BOTH: return true;
	default: return true;
	}
}

static void
get_color_scale_bias(const struct xrt_layer_data *data, float color_scale[4], float color_bias[4])
{
	bool has_color_scale_bias = (data->flags & XRT_LAYER_COMPOSITION_COLOR_BIAS_SCALE) != 0;

	if (has_color_scale_bias) {
		color_scale[0] = data->color_scale.r;
		color_scale[1] = data->color_scale.g;
		color_scale[2] = data->color_scale.b;
		color_scale[3] = data->color_scale.a;
		color_bias[0] = data->color_bias.r;
		color_bias[1] = data->color_bias.g;
		color_bias[2] = data->color_bias.b;
		color_bias[3] = data->color_bias.a;
	} else {
		// Default: no color modification
		color_scale[0] = 1.0f;
		color_scale[1] = 1.0f;
		color_scale[2] = 1.0f;
		color_scale[3] = 1.0f;
		color_bias[0] = 0.0f;
		color_bias[1] = 0.0f;
		color_bias[2] = 0.0f;
		color_bias[3] = 0.0f;
	}
}

static void
render_quad_layer(struct comp_d3d11_renderer *r,
                  const struct comp_layer *layer,
                  uint32_t view_index,
                  const struct xrt_pose *view_pose,
                  const struct xrt_fov *fov)
{
	auto internals = get_internals(r->c);
	const struct xrt_layer_data *data = &layer->data;
	const struct xrt_layer_quad_data *q = &data->quad;

	// Check visibility for this eye
	if (!is_layer_view_visible(data, view_index)) {
		return;
	}

	// Get swapchain
	struct xrt_swapchain *xsc = layer->sc_array[0];
	if (xsc == nullptr) {
		return;
	}

	uint32_t image_index = q->sub.image_index;

	// Get the D3D11 swapchain's SRV for this image
	ID3D11ShaderResourceView *srv = static_cast<ID3D11ShaderResourceView *>(
	    comp_d3d11_swapchain_get_srv(xsc, image_index));
	if (srv == nullptr) {
		return;
	}

	// Build MVP matrix
	struct xrt_matrix_4x4 model, view, proj, mv, mvp;

	// Model: translate + rotate + scale by quad size
	struct xrt_vec3 scale = {q->size.x, q->size.y, 1.0f};
	math_matrix_4x4_model(&q->pose, &scale, &model);

	// View matrix
	math_matrix_4x4_view_from_pose(view_pose, &view);

	// Projection matrix (Vulkan-style infinite reverse)
	math_matrix_4x4_projection_vulkan_infinite_reverse(fov, 0.1f, &proj);

	// MVP
	math_matrix_4x4_multiply(&view, &model, &mv);
	math_matrix_4x4_multiply(&proj, &mv, &mvp);

	// Fill constant buffer
	LayerConstants constants = {};
	memcpy(constants.mvp, &mvp, sizeof(constants.mvp));

	// UV transform for sub-image
	constants.post_transform[0] = q->sub.norm_rect.x;
	constants.post_transform[1] = q->sub.norm_rect.y;
	constants.post_transform[2] = q->sub.norm_rect.w;
	constants.post_transform[3] = q->sub.norm_rect.h;

	// Handle Y-flip
	if (data->flip_y) {
		constants.post_transform[1] += constants.post_transform[3];
		constants.post_transform[3] = -constants.post_transform[3];
	}

	get_color_scale_bias(data, constants.color_scale, constants.color_bias);

	// Update constant buffer
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = internals->context->Map(r->constant_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (SUCCEEDED(hr)) {
		memcpy(mapped.pData, &constants, sizeof(constants));
		internals->context->Unmap(r->constant_buffer, 0);
	}

	// Set shaders - use quad shaders for proper 3D positioning
	internals->context->VSSetShader(r->quad_vs, nullptr, 0);
	internals->context->PSSetShader(r->quad_ps, nullptr, 0);

	// Bind resources
	internals->context->VSSetConstantBuffers(0, 1, &r->constant_buffer);
	internals->context->PSSetConstantBuffers(0, 1, &r->constant_buffer);
	internals->context->PSSetShaderResources(0, 1, &srv);
	internals->context->PSSetSamplers(0, 1, &r->sampler_linear);

	// Set blend state for alpha blending (quads often have transparent areas)
	bool is_premultiplied = (data->flags & XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT) == 0;
	if (is_premultiplied) {
		internals->context->OMSetBlendState(r->blend_premul, nullptr, 0xFFFFFFFF);
	} else {
		internals->context->OMSetBlendState(r->blend_alpha, nullptr, 0xFFFFFFFF);
	}

	// Draw quad (triangle strip, 4 vertices)
	internals->context->Draw(4, 0);

	// Unbind SRV
	ID3D11ShaderResourceView *null_srv = nullptr;
	internals->context->PSSetShaderResources(0, 1, &null_srv);

	// Restore opaque blend state for subsequent layers
	internals->context->OMSetBlendState(r->blend_opaque, nullptr, 0xFFFFFFFF);
}

/*!
 * Render a window-space layer. Positioned in fractional window coordinates
 * with per-eye disparity shift. Uses the same quad shaders.
 */
/*!
 * Render a window-space layer. Positioned in fractional window coordinates.
 * Rendered post-weave onto the back buffer (no per-eye disparity).
 */
static void
render_window_space_layer(struct comp_d3d11_renderer *r,
                          const struct comp_layer *layer)
{
	auto internals = get_internals(r->c);
	const struct xrt_layer_data *data = &layer->data;
	const struct xrt_layer_window_space_data *ws = &data->window_space;

	// Get swapchain
	struct xrt_swapchain *xsc = layer->sc_array[0];
	if (xsc == nullptr) {
		return;
	}

	uint32_t image_index = ws->sub.image_index;

	// Get the D3D11 swapchain's SRV for this image
	ID3D11ShaderResourceView *srv = static_cast<ID3D11ShaderResourceView *>(
	    comp_d3d11_swapchain_get_srv(xsc, image_index));
	if (srv == nullptr) {
		return;
	}

	// Window-space fractional coords → NDC [-1, 1]
	// Center of the quad in fractional window coords
	float frac_cx = ws->x + ws->width / 2.0f;
	float frac_cy = ws->y + ws->height / 2.0f;

	// Convert to NDC: x: frac*2-1, y: 1-frac*2 (Y is flipped in NDC)
	float ndc_cx = frac_cx * 2.0f - 1.0f;
	float ndc_cy = 1.0f - frac_cy * 2.0f;

	// Scale in NDC (full window = 2.0 in NDC)
	float ndc_sx = ws->width * 2.0f;
	float ndc_sy = -(ws->height * 2.0f); // Negate to flip Y: D3D11 texture origin is top-left

	// Build 2D orthographic MVP: scale then translate
	// The quad vertex shader uses a [-0.5, 0.5] unit quad
	// MVP = translate(cx, cy, 0.5) * scale(sx, sy, 1)
	struct xrt_matrix_4x4 mvp;
	// clang-format off
	mvp.v[0]  = ndc_sx; mvp.v[1]  = 0.0f;   mvp.v[2]  = 0.0f; mvp.v[3]  = 0.0f;
	mvp.v[4]  = 0.0f;   mvp.v[5]  = ndc_sy;  mvp.v[6]  = 0.0f; mvp.v[7]  = 0.0f;
	mvp.v[8]  = 0.0f;   mvp.v[9]  = 0.0f;   mvp.v[10] = 1.0f; mvp.v[11] = 0.0f;
	mvp.v[12] = ndc_cx; mvp.v[13] = ndc_cy;  mvp.v[14] = 0.5f; mvp.v[15] = 1.0f;
	// clang-format on

	// Fill constant buffer
	LayerConstants constants = {};
	memcpy(constants.mvp, &mvp, sizeof(constants.mvp));

	// UV transform for sub-image
	constants.post_transform[0] = ws->sub.norm_rect.x;
	constants.post_transform[1] = ws->sub.norm_rect.y;
	constants.post_transform[2] = ws->sub.norm_rect.w;
	constants.post_transform[3] = ws->sub.norm_rect.h;

	// Handle Y-flip
	if (data->flip_y) {
		constants.post_transform[1] += constants.post_transform[3];
		constants.post_transform[3] = -constants.post_transform[3];
	}

	get_color_scale_bias(data, constants.color_scale, constants.color_bias);

	// Update constant buffer
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = internals->context->Map(r->constant_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (SUCCEEDED(hr)) {
		memcpy(mapped.pData, &constants, sizeof(constants));
		internals->context->Unmap(r->constant_buffer, 0);
	}

	// Set shaders - reuse quad shaders (screen-aligned quad with MVP)
	internals->context->VSSetShader(r->quad_vs, nullptr, 0);
	internals->context->PSSetShader(r->quad_ps, nullptr, 0);

	// Bind resources
	internals->context->VSSetConstantBuffers(0, 1, &r->constant_buffer);
	internals->context->PSSetConstantBuffers(0, 1, &r->constant_buffer);
	internals->context->PSSetShaderResources(0, 1, &srv);
	internals->context->PSSetSamplers(0, 1, &r->sampler_linear);

	// Set blend state for alpha blending
	bool is_premultiplied = (data->flags & XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT) == 0;
	if (is_premultiplied) {
		internals->context->OMSetBlendState(r->blend_premul, nullptr, 0xFFFFFFFF);
	} else {
		internals->context->OMSetBlendState(r->blend_alpha, nullptr, 0xFFFFFFFF);
	}

	// Draw quad (triangle strip, 4 vertices)
	internals->context->Draw(4, 0);

	// Unbind SRV
	ID3D11ShaderResourceView *null_srv = nullptr;
	internals->context->PSSetShaderResources(0, 1, &null_srv);

	// Restore opaque blend state for subsequent layers
	internals->context->OMSetBlendState(r->blend_opaque, nullptr, 0xFFFFFFFF);
}

extern "C" xrt_result_t
comp_d3d11_renderer_create(struct comp_d3d11_compositor *c,
                           uint32_t view_width,
                           uint32_t view_height,
                           uint32_t target_height,
                           struct comp_d3d11_renderer **out_renderer)
{
	comp_d3d11_renderer *r = new comp_d3d11_renderer();
	memset(r, 0, sizeof(*r));

	r->c = c;
	r->view_width = view_width;
	r->view_height = view_height;

	// Initialize tile layout from the active rendering mode
	auto ci = get_internals(c);
	if (ci->xdev != NULL && ci->xdev->hmd != NULL) {
		uint32_t idx = ci->xdev->hmd->active_rendering_mode_index;
		if (idx < ci->xdev->rendering_mode_count) {
			r->tile_columns = ci->xdev->rendering_modes[idx].tile_columns;
			r->tile_rows = ci->xdev->rendering_modes[idx].tile_rows;
		}
	}
	// Default to 2x1 (stereo) if not set
	if (r->tile_columns == 0) {
		r->tile_columns = 2;
	}
	if (r->tile_rows == 0) {
		r->tile_rows = 1;
	}

	// Texture height must accommodate tile_rows * view_height for
	// multi-row layouts, and at least target_height for mono fallback.
	uint32_t atlas_h = r->tile_rows * view_height;
	uint32_t min_h = (target_height > atlas_h) ? target_height : atlas_h;
	r->texture_height = (min_h > view_height) ? min_h : view_height;

	xrt_result_t xret = create_shaders(r);
	if (xret != XRT_SUCCESS) {
		delete r;
		return xret;
	}

	xret = create_resources(r);
	if (xret != XRT_SUCCESS) {
		comp_d3d11_renderer_destroy(&r);
		return xret;
	}

	*out_renderer = r;

	U_LOG_I("Created D3D11 renderer: view=%ux%u, tiles=%ux%u, tex_h=%u (target_h=%u)",
	        view_width, view_height, r->tile_columns, r->tile_rows,
	        r->texture_height, target_height);

	return XRT_SUCCESS;
}

extern "C" void
comp_d3d11_renderer_destroy(struct comp_d3d11_renderer **renderer_ptr)
{
	if (renderer_ptr == nullptr || *renderer_ptr == nullptr) {
		return;
	}

	comp_d3d11_renderer *r = *renderer_ptr;

#define SAFE_RELEASE(x)                                                                                                \
	if (x != nullptr) {                                                                                            \
		x->Release();                                                                                          \
		x = nullptr;                                                                                           \
	}

	SAFE_RELEASE(r->depth_stencil_state);
	SAFE_RELEASE(r->rasterizer_state);
	SAFE_RELEASE(r->blend_opaque);
	SAFE_RELEASE(r->blend_premul);
	SAFE_RELEASE(r->blend_alpha);
	SAFE_RELEASE(r->sampler_point);
	SAFE_RELEASE(r->sampler_linear);
	SAFE_RELEASE(r->constant_buffer);
	SAFE_RELEASE(r->quad_ps);
	SAFE_RELEASE(r->quad_vs);
	SAFE_RELEASE(r->projection_ps);
	SAFE_RELEASE(r->projection_vs);
	SAFE_RELEASE(r->depth_dsv);
	SAFE_RELEASE(r->depth_texture);
	SAFE_RELEASE(r->atlas_rtv);
	SAFE_RELEASE(r->atlas_srv);
	SAFE_RELEASE(r->atlas_texture);

#undef SAFE_RELEASE

	delete r;
	*renderer_ptr = nullptr;
}

extern "C" xrt_result_t
comp_d3d11_renderer_draw(struct comp_d3d11_renderer *renderer,
                         struct comp_layer_accum *layers,
                         struct xrt_vec3 *left_eye,
                         struct xrt_vec3 *right_eye,
                         uint32_t target_width,
                         uint32_t target_height,
                         bool hardware_display_3d)
{
	auto internals = get_internals(renderer->c);

	// Set render target to atlas texture
	internals->context->OMSetRenderTargets(1, &renderer->atlas_rtv, renderer->depth_dsv);

	// Clear to dark blue (similar to Vulkan compositor)
	float clear_color[4] = {0.05f, 0.05f, 0.25f, 1.0f};
	internals->context->ClearRenderTargetView(renderer->atlas_rtv, clear_color);
	internals->context->ClearDepthStencilView(renderer->depth_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f,
	                                           0);

	// Set common state
	internals->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	internals->context->IASetInputLayout(nullptr); // No vertex buffer, using SV_VertexID
	internals->context->VSSetShader(renderer->projection_vs, nullptr, 0);
	internals->context->PSSetShader(renderer->projection_ps, nullptr, 0);
	internals->context->RSSetState(renderer->rasterizer_state);
	internals->context->OMSetDepthStencilState(renderer->depth_stencil_state, 0);
	internals->context->OMSetBlendState(renderer->blend_opaque, nullptr, 0xFFFFFFFF);

	// Set up default view poses and FOVs for UI layer rendering
	struct xrt_pose view_poses[2];
	struct xrt_fov fovs[2];

	// Default identity pose with slight IPD offset
	view_poses[0].orientation.x = 0.0f;
	view_poses[0].orientation.y = 0.0f;
	view_poses[0].orientation.z = 0.0f;
	view_poses[0].orientation.w = 1.0f;
	view_poses[0].position.x = -0.032f; // IPD/2
	view_poses[0].position.y = 0.0f;
	view_poses[0].position.z = 0.0f;

	view_poses[1].orientation.x = 0.0f;
	view_poses[1].orientation.y = 0.0f;
	view_poses[1].orientation.z = 0.0f;
	view_poses[1].orientation.w = 1.0f;
	view_poses[1].position.x = 0.032f; // IPD/2
	view_poses[1].position.y = 0.0f;
	view_poses[1].position.z = 0.0f;

	// Default symmetric FOV (roughly 90 degrees)
	const float fov_angle = 0.785f; // ~45 degrees
	for (uint32_t view = 0; view < 2; view++) {
		fovs[view].angle_left = -fov_angle;
		fovs[view].angle_right = fov_angle;
		fovs[view].angle_up = fov_angle;
		fovs[view].angle_down = -fov_angle;
	}

	// Determine effective view count from first projection layer.
	// In 2D mode (!hardware_display_3d), override to 1 view.
	uint32_t effective_views = 2;
	for (uint32_t i = 0; i < layers->layer_count; i++) {
		if (layers->layers[i].data.type == XRT_LAYER_PROJECTION ||
		    layers->layers[i].data.type == XRT_LAYER_PROJECTION_DEPTH) {
			effective_views = layers->layers[i].data.view_count;
			break;
		}
	}
	if (!hardware_display_3d && effective_views > 1) {
		effective_views = 1;
	}

	// Render each view (1 for mono, 2+ for stereo/multi-view)
	for (uint32_t view_index = 0; view_index < effective_views; view_index++) {
		// Set viewport for this view
		D3D11_VIEWPORT viewport = {};
		if (effective_views == 1) {
			// MONO: use target (window) dimensions so 2D content fills
			// the full window. Width is capped to the atlas texture width
			// (tile_columns*view_width); height is capped to texture_height.
			uint32_t atlas_w = renderer->tile_columns * renderer->view_width;
			uint32_t mono_w = (target_width < atlas_w)
			                      ? target_width
			                      : atlas_w;
			uint32_t mono_h = (target_height < renderer->texture_height)
			                      ? target_height
			                      : renderer->texture_height;
			viewport.TopLeftX = 0.0f;
			viewport.Width = static_cast<float>(mono_w);
			viewport.Height = static_cast<float>(mono_h);
		} else {
			// MULTI-VIEW: tile-based atlas layout
			uint32_t col = view_index % renderer->tile_columns;
			uint32_t row = view_index / renderer->tile_columns;
			viewport.TopLeftX = static_cast<float>(col * renderer->view_width);
			viewport.TopLeftY = static_cast<float>(row * renderer->view_height);
			viewport.Width = static_cast<float>(renderer->view_width);
			viewport.Height = static_cast<float>(renderer->view_height);
		}
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		internals->context->RSSetViewports(1, &viewport);

		struct xrt_vec3 *eye = (view_index == 0) ? left_eye : right_eye;

		// Render all layers for this view
		for (uint32_t i = 0; i < layers->layer_count; i++) {
			struct comp_layer *layer = &layers->layers[i];

			switch (layer->data.type) {
			case XRT_LAYER_PROJECTION:
			case XRT_LAYER_PROJECTION_DEPTH:
				render_projection_layer(renderer, layer, view_index, eye);
				break;

			case XRT_LAYER_QUAD:
				render_quad_layer(renderer, layer, view_index, &view_poses[view_index],
				                  &fovs[view_index]);
				break;

			case XRT_LAYER_CYLINDER: {
				static bool cylinder_warned = false;
				if (!cylinder_warned) {
					U_LOG_W("Cylinder layers not yet implemented in D3D11 compositor");
					cylinder_warned = true;
				}
				break;
			}

			case XRT_LAYER_EQUIRECT1:
			case XRT_LAYER_EQUIRECT2: {
				static bool equirect_warned = false;
				if (!equirect_warned) {
					U_LOG_W("Equirect layers not yet implemented in D3D11 compositor");
					equirect_warned = true;
				}
				break;
			}

			case XRT_LAYER_CUBE: {
				static bool cube_warned = false;
				if (!cube_warned) {
					U_LOG_W("Cube layers not yet implemented in D3D11 compositor");
					cube_warned = true;
				}
				break;
			}

			case XRT_LAYER_WINDOW_SPACE:
				// Rendered post-weave by compositor, not into the atlas
				break;

			default:
				// Unsupported layer type
				break;
			}
		}
	}

	return XRT_SUCCESS;
}

extern "C" void *
comp_d3d11_renderer_get_atlas_srv(struct comp_d3d11_renderer *renderer)
{
	return renderer->atlas_srv;
}

extern "C" void *
comp_d3d11_renderer_get_atlas_rtv(struct comp_d3d11_renderer *renderer)
{
	return renderer->atlas_rtv;
}

extern "C" void
comp_d3d11_renderer_get_view_dimensions(struct comp_d3d11_renderer *renderer,
                                        uint32_t *out_view_width,
                                        uint32_t *out_view_height)
{
	*out_view_width = renderer->view_width;
	*out_view_height = renderer->view_height;
}

extern "C" void
comp_d3d11_renderer_get_tile_layout(struct comp_d3d11_renderer *renderer,
                                    uint32_t *out_tile_columns,
                                    uint32_t *out_tile_rows)
{
	*out_tile_columns = renderer->tile_columns;
	*out_tile_rows = renderer->tile_rows;
}

extern "C" void
comp_d3d11_renderer_set_tile_layout(struct comp_d3d11_renderer *renderer,
                                    uint32_t tile_columns,
                                    uint32_t tile_rows)
{
	if (!renderer->legacy_app_tile_scaling) {
		// Extension app: recompute view dimensions so the atlas logical size stays constant.
		// E.g. stereo 2×1 (vw=1920) → 2D 1×1 (vw=3840) keeps atlas_w=3840.
		if (tile_columns > 0 && renderer->tile_columns > 0) {
			uint32_t atlas_w = renderer->tile_columns * renderer->view_width;
			renderer->view_width = atlas_w / tile_columns;
		}
		if (tile_rows > 0 && renderer->tile_rows > 0) {
			uint32_t atlas_h = renderer->tile_rows * renderer->view_height;
			renderer->view_height = atlas_h / tile_rows;
		}
	}
	// Legacy app: view dims stay fixed at compromise scale.
	// Only update tile layout — the app always renders the same atlas.
	renderer->tile_columns = tile_columns;
	renderer->tile_rows = tile_rows;
}

extern "C" void
comp_d3d11_renderer_set_legacy_app_tile_scaling(struct comp_d3d11_renderer *renderer,
                                                 bool legacy)
{
	if (renderer != nullptr) {
		renderer->legacy_app_tile_scaling = legacy;
	}
}

extern "C" void *
comp_d3d11_renderer_get_atlas_texture(struct comp_d3d11_renderer *renderer)
{
	return renderer->atlas_texture;
}

extern "C" xrt_result_t
comp_d3d11_renderer_resize(struct comp_d3d11_renderer *renderer,
                           uint32_t new_view_width,
                           uint32_t new_view_height,
                           uint32_t new_target_height)
{
	if (renderer == nullptr) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Clamp minimum
	if (new_view_width < 64) {
		new_view_width = 64;
	}
	if (new_view_height < 64) {
		new_view_height = 64;
	}

	uint32_t atlas_h = renderer->tile_rows * new_view_height;
	uint32_t min_h = (new_target_height > atlas_h) ? new_target_height : atlas_h;
	uint32_t new_texture_height = (min_h > new_view_height) ? min_h : new_view_height;

	// Skip if unchanged
	if (new_view_width == renderer->view_width &&
	    new_view_height == renderer->view_height &&
	    new_texture_height == renderer->texture_height) {
		return XRT_SUCCESS;
	}

	auto internals = get_internals(renderer->c);

	U_LOG_W("Renderer resize: view %ux%u -> %ux%u, tex_h %u -> %u",
	        renderer->view_width, renderer->view_height,
	        new_view_width, new_view_height,
	        renderer->texture_height, new_texture_height);

	// Release existing resources
#define SAFE_RELEASE(x)                                                                                                \
	if (x != nullptr) {                                                                                            \
		x->Release();                                                                                          \
		x = nullptr;                                                                                           \
	}

	SAFE_RELEASE(renderer->depth_dsv);
	SAFE_RELEASE(renderer->depth_texture);
	SAFE_RELEASE(renderer->atlas_rtv);
	SAFE_RELEASE(renderer->atlas_srv);
	SAFE_RELEASE(renderer->atlas_texture);

#undef SAFE_RELEASE

	// Update dimensions
	renderer->view_width = new_view_width;
	renderer->view_height = new_view_height;
	renderer->texture_height = new_texture_height;

	// Recreate atlas texture (tile_columns * view_width)
	D3D11_TEXTURE2D_DESC texDesc = {};
	texDesc.Width = renderer->tile_columns * new_view_width;
	texDesc.Height = new_texture_height;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = 1;
	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	texDesc.SampleDesc.Count = 1;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

	HRESULT hr = internals->device->CreateTexture2D(&texDesc, nullptr, &renderer->atlas_texture);
	if (FAILED(hr)) {
		U_LOG_E("Failed to recreate atlas texture: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Recreate SRV
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = texDesc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;

	hr = internals->device->CreateShaderResourceView(renderer->atlas_texture, &srvDesc, &renderer->atlas_srv);
	if (FAILED(hr)) {
		U_LOG_E("Failed to recreate atlas SRV: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Recreate RTV
	hr = internals->device->CreateRenderTargetView(renderer->atlas_texture, nullptr, &renderer->atlas_rtv);
	if (FAILED(hr)) {
		U_LOG_E("Failed to recreate atlas RTV: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Recreate depth texture
	texDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

	hr = internals->device->CreateTexture2D(&texDesc, nullptr, &renderer->depth_texture);
	if (FAILED(hr)) {
		U_LOG_E("Failed to recreate depth texture: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Recreate DSV
	hr = internals->device->CreateDepthStencilView(renderer->depth_texture, nullptr, &renderer->depth_dsv);
	if (FAILED(hr)) {
		U_LOG_E("Failed to recreate depth DSV: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	U_LOG_W("Renderer resized: atlas texture now %ux%u (view=%ux%u, tiles=%ux%u, tex_h=%u)",
	        renderer->tile_columns * new_view_width, new_texture_height,
	        new_view_width, new_view_height,
	        renderer->tile_columns, renderer->tile_rows, new_texture_height);

	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d11_renderer_blit_stretch(struct comp_d3d11_renderer *renderer,
                                 void *back_buffer_texture,
                                 uint32_t target_width,
                                 uint32_t target_height)
{
	if (renderer == nullptr || back_buffer_texture == nullptr) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	auto internals = get_internals(renderer->c);
	ID3D11Texture2D *bb = static_cast<ID3D11Texture2D *>(back_buffer_texture);

	// Create temporary RTV for the back buffer
	ID3D11RenderTargetView *rtv = nullptr;
	HRESULT hr = internals->device->CreateRenderTargetView(bb, nullptr, &rtv);
	if (FAILED(hr)) {
		U_LOG_E("blit_stretch: failed to create RTV: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Bind back buffer as render target (no depth)
	internals->context->OMSetRenderTargets(1, &rtv, nullptr);

	// Set viewport to fill the entire back buffer
	D3D11_VIEWPORT vp = {};
	vp.Width = static_cast<float>(target_width);
	vp.Height = static_cast<float>(target_height);
	vp.MaxDepth = 1.0f;
	internals->context->RSSetViewports(1, &vp);

	// Set pipeline state (reuse renderer's existing objects)
	internals->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	internals->context->IASetInputLayout(nullptr);
	internals->context->VSSetShader(renderer->projection_vs, nullptr, 0);
	internals->context->PSSetShader(renderer->projection_ps, nullptr, 0);
	internals->context->RSSetState(renderer->rasterizer_state);
	internals->context->OMSetDepthStencilState(renderer->depth_stencil_state, 0);
	internals->context->OMSetBlendState(renderer->blend_opaque, nullptr, 0xFFFFFFFF);
	internals->context->PSSetSamplers(0, 1, &renderer->sampler_linear);

	// Bind atlas texture SRV
	internals->context->PSSetShaderResources(0, 1, &renderer->atlas_srv);

	// Set constant buffer: identity MVP, UV covers the mono-rendered region.
	// In mono mode, the rendered content occupies the top-left
	// min(target_w, atlas_w) x min(target_h, texture_h) of the atlas texture.
	// If the atlas texture is larger than the mono viewport (e.g. SR recommended
	// dims exceed window size at initial creation), we must restrict the UV range
	// to avoid sampling unrendered texels — otherwise the content appears squished.
	LayerConstants constants = {};
	// Identity MVP (fullscreen quad in NDC)
	constants.mvp[0] = 1.0f;
	constants.mvp[5] = 1.0f;
	constants.mvp[10] = 1.0f;
	constants.mvp[15] = 1.0f;
	// UV transform: sample only the mono-rendered portion of the atlas texture.
	// Content occupies tile_columns*view_width x texture_height of the atlas,
	// but the atlas physical size may be larger (e.g. 1152px wide in 3D mode).
	// UV must be content_width / atlas_physical_width to avoid sampling black.
	uint32_t tex_w = renderer->tile_columns * renderer->view_width;
	uint32_t tex_h = renderer->texture_height;
	D3D11_TEXTURE2D_DESC atlas_desc;
	renderer->atlas_texture->GetDesc(&atlas_desc);
	float u_scale = (atlas_desc.Width > 0) ? (float)tex_w / (float)atlas_desc.Width : 1.0f;
	float v_scale = (atlas_desc.Height > 0) ? (float)tex_h / (float)atlas_desc.Height : 1.0f;
	constants.post_transform[0] = 0.0f;    // x offset
	constants.post_transform[1] = 0.0f;    // y offset
	constants.post_transform[2] = u_scale;  // width scale
	constants.post_transform[3] = v_scale;  // height scale
	// Color identity
	constants.color_scale[0] = 1.0f;
	constants.color_scale[1] = 1.0f;
	constants.color_scale[2] = 1.0f;
	constants.color_scale[3] = 1.0f;

	D3D11_MAPPED_SUBRESOURCE mapped;
	hr = internals->context->Map(renderer->constant_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (SUCCEEDED(hr)) {
		memcpy(mapped.pData, &constants, sizeof(constants));
		internals->context->Unmap(renderer->constant_buffer, 0);
	}
	internals->context->VSSetConstantBuffers(0, 1, &renderer->constant_buffer);
	internals->context->PSSetConstantBuffers(0, 1, &renderer->constant_buffer);

	// Draw fullscreen quad (triangle strip, 4 vertices)
	internals->context->Draw(4, 0);

	// Unbind SRV to prevent hazard warnings
	ID3D11ShaderResourceView *null_srv = nullptr;
	internals->context->PSSetShaderResources(0, 1, &null_srv);

	rtv->Release();

	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d11_renderer_draw_window_space_layers(struct comp_d3d11_renderer *renderer,
                                              struct comp_layer_accum *layers,
                                              uint32_t target_width,
                                              uint32_t target_height)
{
	if (renderer == nullptr || layers == nullptr) {
		return XRT_SUCCESS;
	}

	// Check if there are any window-space layers to render
	bool has_ws = false;
	for (uint32_t i = 0; i < layers->layer_count; i++) {
		if (layers->layers[i].data.type == XRT_LAYER_WINDOW_SPACE) {
			has_ws = true;
			break;
		}
	}
	if (!has_ws) {
		return XRT_SUCCESS;
	}

	auto internals = get_internals(renderer->c);

	// Set full-target viewport (window-space layers use fractional [0,1] coords)
	D3D11_VIEWPORT viewport = {};
	viewport.Width = static_cast<float>(target_width);
	viewport.Height = static_cast<float>(target_height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	internals->context->RSSetViewports(1, &viewport);

	// Restore pipeline state (may have been changed by display processor)
	internals->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	internals->context->IASetInputLayout(nullptr);
	internals->context->RSSetState(renderer->rasterizer_state);
	internals->context->OMSetDepthStencilState(renderer->depth_stencil_state, 0);
	internals->context->OMSetBlendState(renderer->blend_opaque, nullptr, 0xFFFFFFFF);

	for (uint32_t i = 0; i < layers->layer_count; i++) {
		struct comp_layer *layer = &layers->layers[i];
		if (layer->data.type == XRT_LAYER_WINDOW_SPACE) {
			render_window_space_layer(renderer, layer);
		}
	}

	return XRT_SUCCESS;
}
