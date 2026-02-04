// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 service compositor implementation.
 * @author David Fattal
 * @ingroup comp_d3d11_service
 */

#include "comp_d3d11_service.h"
#include "d3d11_service_shaders.h"

#include "xrt/xrt_handles.h"
#include "xrt/xrt_config_have.h"
#include "xrt/xrt_limits.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "os/os_time.h"

#include "util/comp_layer_accum.h"

#include "comp_d3d11_window.h"

#include "math/m_api.h"
#include "math/m_vec3.h"

#include "d3d/d3d_d3d11_fence.hpp"
#include "d3d/d3d_dxgi_formats.h"

#ifdef XRT_HAVE_LEIA_SR_D3D11
#include "leiasr/leiasr_d3d11.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

#include <wil/com.h>
#include <wil/result.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <mutex>


/*
 *
 * Structures
 *
 */

/*!
 * Swapchain image with KeyedMutex synchronization.
 */
struct d3d11_service_image
{
	//! The imported texture
	wil::com_ptr<ID3D11Texture2D> texture;

	//! Shader resource view for compositing
	wil::com_ptr<ID3D11ShaderResourceView> srv;

	//! KeyedMutex for cross-process synchronization
	wil::com_ptr<IDXGIKeyedMutex> keyed_mutex;

	//! Whether we currently hold the mutex
	bool mutex_acquired;
};

/*!
 * D3D11 service swapchain.
 *
 * For service-created swapchains (WebXR), we use xrt_swapchain_native as base
 * so the IPC layer can access the shared handles to send to the client.
 */
struct d3d11_service_swapchain
{
	//! Base native swapchain - must be first!
	//! Contains xrt_swapchain + images[] with shared handles for IPC
	struct xrt_swapchain_native base;

	//! Parent compositor
	struct d3d11_service_compositor *comp;

	//! Swapchain images (compositor's view of the textures)
	struct d3d11_service_image images[XRT_MAX_SWAPCHAIN_IMAGES];

	//! Image count
	uint32_t image_count;

	//! Creation info
	struct xrt_swapchain_create_info info;

	//! Whether this swapchain was created by the service (vs imported from client)
	bool service_created;
};

/*!
 * D3D11 service native compositor.
 */
struct d3d11_service_compositor
{
	//! Base native compositor - must be first!
	struct xrt_compositor_native base;

	//! Parent system compositor
	struct d3d11_service_system *sys;

	//! Accumulated layers for the current frame
	struct comp_layer_accum layer_accum;

	//! Logging level
	enum u_logging_level log_level;

	//! Frame ID
	int64_t frame_id;

	//! Thread safety
	std::mutex mutex;
};

/*!
 * D3D11 service compositor semaphore (timeline fence).
 */
struct d3d11_service_semaphore
{
	//! Base semaphore - must be first!
	struct xrt_compositor_semaphore base;

	//! Parent system
	struct d3d11_service_system *sys;

	//! The D3D11 fence
	wil::com_ptr<ID3D11Fence> fence;

	//! Event for waiting on fence
	wil::unique_event_nothrow wait_event;
};


/*!
 * D3D11 service system compositor.
 */
struct d3d11_service_system
{
	//! Base system compositor - must be first!
	struct xrt_system_compositor base;

	//! The device we are rendering for
	struct xrt_device *xdev;

	//! D3D11 device (owned by service, not the app)
	wil::com_ptr<ID3D11Device5> device;

	//! D3D11 immediate context
	wil::com_ptr<ID3D11DeviceContext4> context;

	//! DXGI factory
	wil::com_ptr<IDXGIFactory4> dxgi_factory;

	//! DXGI swap chain for display output
	wil::com_ptr<IDXGISwapChain1> swap_chain;

	//! Back buffer render target view
	wil::com_ptr<ID3D11RenderTargetView> back_buffer_rtv;

	//! Stereo render target (side-by-side views)
	wil::com_ptr<ID3D11Texture2D> stereo_texture;
	wil::com_ptr<ID3D11ShaderResourceView> stereo_srv;
	wil::com_ptr<ID3D11RenderTargetView> stereo_rtv;

	//! Quad layer shaders
	wil::com_ptr<ID3D11VertexShader> quad_vs;
	wil::com_ptr<ID3D11PixelShader> quad_ps;

	//! Cylinder layer shaders
	wil::com_ptr<ID3D11VertexShader> cylinder_vs;
	wil::com_ptr<ID3D11PixelShader> cylinder_ps;

	//! Equirect2 layer shaders
	wil::com_ptr<ID3D11VertexShader> equirect2_vs;
	wil::com_ptr<ID3D11PixelShader> equirect2_ps;

	//! Cube layer shaders
	wil::com_ptr<ID3D11VertexShader> cube_vs;
	wil::com_ptr<ID3D11PixelShader> cube_ps;

	//! Constant buffer for layer rendering
	wil::com_ptr<ID3D11Buffer> layer_constant_buffer;

	//! Linear sampler for layer textures
	wil::com_ptr<ID3D11SamplerState> sampler_linear;

	//! Blend state for alpha blending
	wil::com_ptr<ID3D11BlendState> blend_alpha;

	//! Blend state for premultiplied alpha
	wil::com_ptr<ID3D11BlendState> blend_premul;

	//! Blend state for opaque
	wil::com_ptr<ID3D11BlendState> blend_opaque;

	//! Rasterizer state for layer rendering
	wil::com_ptr<ID3D11RasterizerState> rasterizer_state;

	//! Depth stencil state (disabled)
	wil::com_ptr<ID3D11DepthStencilState> depth_disabled;

	//! Dedicated-thread window (owns HWND and message pump)
	struct comp_d3d11_window *window;

	//! Cached HWND from window (for swap chain, weaver)
	HWND hwnd;

#ifdef XRT_HAVE_LEIA_SR_D3D11
	//! SR weaver for light field display
	struct leiasr_d3d11 *weaver;
#endif

	//! Stereo texture dimensions (side-by-side views, input to weaver)
	uint32_t display_width;
	uint32_t display_height;

	//! Output dimensions (window/swap chain, native display resolution)
	uint32_t output_width;
	uint32_t output_height;

	//! View dimensions (per eye, reported to apps)
	uint32_t view_width;
	uint32_t view_height;

	//! Display refresh rate
	float refresh_rate;

	//! Logging level
	enum u_logging_level log_level;
};


/*
 *
 * Helper functions
 *
 */

static inline struct d3d11_service_swapchain *
d3d11_service_swapchain_from_xrt(struct xrt_swapchain *xsc)
{
	return reinterpret_cast<struct d3d11_service_swapchain *>(xsc);
}

static inline struct d3d11_service_compositor *
d3d11_service_compositor_from_xrt(struct xrt_compositor *xc)
{
	return reinterpret_cast<struct d3d11_service_compositor *>(xc);
}

static inline struct d3d11_service_system *
d3d11_service_system_from_xrt(struct xrt_system_compositor *xsysc)
{
	return reinterpret_cast<struct d3d11_service_system *>(xsysc);
}

static inline struct d3d11_service_semaphore *
d3d11_service_semaphore_from_xrt(struct xrt_compositor_semaphore *xcsem)
{
	return reinterpret_cast<struct d3d11_service_semaphore *>(xcsem);
}


/*
 *
 * Shader compilation helpers
 *
 */

static HRESULT
compile_shader(const char *source, const char *entry, const char *target, ID3DBlob **out_blob)
{
	ID3DBlob *errors = nullptr;
	HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, entry, target, 0, 0, out_blob,
	                        &errors);
	if (FAILED(hr)) {
		if (errors != nullptr) {
			U_LOG_E("Shader compile error: %s", (char *)errors->GetBufferPointer());
			errors->Release();
		}
	}
	if (errors != nullptr) {
		errors->Release();
	}
	return hr;
}

static bool
create_layer_shaders(struct d3d11_service_system *sys)
{
	ID3DBlob *blob = nullptr;
	HRESULT hr;

	// Quad vertex shader
	hr = compile_shader(quad_vs_hlsl, "VSMain", "vs_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile quad vertex shader");
		return false;
	}
	hr = sys->device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                      sys->quad_vs.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create quad vertex shader: 0x%08lx", hr);
		return false;
	}

	// Quad pixel shader
	hr = compile_shader(quad_ps_hlsl, "PSMain", "ps_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile quad pixel shader");
		return false;
	}
	hr = sys->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                     sys->quad_ps.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create quad pixel shader: 0x%08lx", hr);
		return false;
	}

	// Cylinder vertex shader
	hr = compile_shader(cylinder_vs_hlsl, "VSMain", "vs_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile cylinder vertex shader");
		return false;
	}
	hr = sys->device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                      sys->cylinder_vs.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create cylinder vertex shader: 0x%08lx", hr);
		return false;
	}

	// Cylinder pixel shader
	hr = compile_shader(cylinder_ps_hlsl, "PSMain", "ps_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile cylinder pixel shader");
		return false;
	}
	hr = sys->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                     sys->cylinder_ps.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create cylinder pixel shader: 0x%08lx", hr);
		return false;
	}

	// Equirect2 vertex shader
	hr = compile_shader(equirect2_vs_hlsl, "VSMain", "vs_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile equirect2 vertex shader");
		return false;
	}
	hr = sys->device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                      sys->equirect2_vs.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create equirect2 vertex shader: 0x%08lx", hr);
		return false;
	}

	// Equirect2 pixel shader
	hr = compile_shader(equirect2_ps_hlsl, "PSMain", "ps_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile equirect2 pixel shader");
		return false;
	}
	hr = sys->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                     sys->equirect2_ps.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create equirect2 pixel shader: 0x%08lx", hr);
		return false;
	}

	// Cube vertex shader
	hr = compile_shader(cube_vs_hlsl, "VSMain", "vs_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile cube vertex shader");
		return false;
	}
	hr = sys->device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                      sys->cube_vs.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create cube vertex shader: 0x%08lx", hr);
		return false;
	}

	// Cube pixel shader
	hr = compile_shader(cube_ps_hlsl, "PSMain", "ps_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile cube pixel shader");
		return false;
	}
	hr = sys->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                     sys->cube_ps.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create cube pixel shader: 0x%08lx", hr);
		return false;
	}

	U_LOG_I("Created all layer shaders");
	return true;
}

static bool
create_layer_resources(struct d3d11_service_system *sys)
{
	HRESULT hr;

	// Create constant buffer (largest of all layer constant structs)
	size_t cb_size = sizeof(Equirect2LayerConstants);  // Largest
	D3D11_BUFFER_DESC cb_desc = {};
	cb_desc.ByteWidth = static_cast<UINT>((cb_size + 15) & ~15);  // 16-byte aligned
	cb_desc.Usage = D3D11_USAGE_DYNAMIC;
	cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	hr = sys->device->CreateBuffer(&cb_desc, nullptr, sys->layer_constant_buffer.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create layer constant buffer: 0x%08lx", hr);
		return false;
	}

	// Create linear sampler
	D3D11_SAMPLER_DESC samp_desc = {};
	samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;

	hr = sys->device->CreateSamplerState(&samp_desc, sys->sampler_linear.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create linear sampler: 0x%08lx", hr);
		return false;
	}

	// Create blend state for alpha blending
	D3D11_BLEND_DESC blend_desc = {};
	blend_desc.RenderTarget[0].BlendEnable = TRUE;
	blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

	hr = sys->device->CreateBlendState(&blend_desc, sys->blend_alpha.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create alpha blend state: 0x%08lx", hr);
		return false;
	}

	// Premultiplied alpha blend state
	blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;

	hr = sys->device->CreateBlendState(&blend_desc, sys->blend_premul.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create premul blend state: 0x%08lx", hr);
		return false;
	}

	// Opaque blend state
	blend_desc.RenderTarget[0].BlendEnable = FALSE;

	hr = sys->device->CreateBlendState(&blend_desc, sys->blend_opaque.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create opaque blend state: 0x%08lx", hr);
		return false;
	}

	// Create rasterizer state
	D3D11_RASTERIZER_DESC raster_desc = {};
	raster_desc.FillMode = D3D11_FILL_SOLID;
	raster_desc.CullMode = D3D11_CULL_NONE;
	raster_desc.FrontCounterClockwise = FALSE;
	raster_desc.DepthClipEnable = TRUE;

	hr = sys->device->CreateRasterizerState(&raster_desc, sys->rasterizer_state.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create rasterizer state: 0x%08lx", hr);
		return false;
	}

	// Create depth stencil state (disabled)
	D3D11_DEPTH_STENCIL_DESC ds_desc = {};
	ds_desc.DepthEnable = FALSE;
	ds_desc.StencilEnable = FALSE;

	hr = sys->device->CreateDepthStencilState(&ds_desc, sys->depth_disabled.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create depth stencil state: 0x%08lx", hr);
		return false;
	}

	U_LOG_I("Created all layer rendering resources");
	return true;
}


/*
 *
 * Layer visibility check
 *
 */

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
	default: return true;  // Projection layers visible in both
	}

	switch (visibility) {
	case XRT_LAYER_EYE_VISIBILITY_NONE: return false;
	case XRT_LAYER_EYE_VISIBILITY_LEFT_BIT: return view_index == 0;
	case XRT_LAYER_EYE_VISIBILITY_RIGHT_BIT: return view_index == 1;
	case XRT_LAYER_EYE_VISIBILITY_BOTH: return true;
	default: return true;
	}
}


/*
 *
 * Layer rendering helpers
 *
 */

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
set_blend_state(struct d3d11_service_system *sys, const struct xrt_layer_data *data)
{
	bool use_premul = (data->flags & XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT) == 0;

	if (use_premul) {
		sys->context->OMSetBlendState(sys->blend_premul.get(), nullptr, 0xFFFFFFFF);
	} else {
		sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
	}
}

static void
render_quad_layer(struct d3d11_service_system *sys,
                  const struct comp_layer *layer,
                  uint32_t view_index,
                  const struct xrt_pose *view_pose,
                  const struct xrt_fov *fov)
{
	const struct xrt_layer_data *data = &layer->data;
	const struct xrt_layer_quad_data *q = &data->quad;

	// Get swapchain
	struct xrt_swapchain *xsc = layer->sc_array[0];
	if (xsc == nullptr) {
		return;
	}
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain_from_xrt(xsc);

	uint32_t image_index = q->sub.image_index;
	if (image_index >= sc->image_count) {
		return;
	}

	ID3D11ShaderResourceView *srv = sc->images[image_index].srv.get();
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
	QuadLayerConstants constants = {};
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
	HRESULT hr = sys->context->Map(sys->layer_constant_buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (SUCCEEDED(hr)) {
		memcpy(mapped.pData, &constants, sizeof(constants));
		sys->context->Unmap(sys->layer_constant_buffer.get(), 0);
	}

	// Set shaders
	sys->context->VSSetShader(sys->quad_vs.get(), nullptr, 0);
	sys->context->PSSetShader(sys->quad_ps.get(), nullptr, 0);

	// Bind resources
	ID3D11Buffer *cbs[] = {sys->layer_constant_buffer.get()};
	sys->context->VSSetConstantBuffers(0, 1, cbs);
	sys->context->PSSetConstantBuffers(0, 1, cbs);
	sys->context->PSSetShaderResources(0, 1, &srv);
	ID3D11SamplerState *samplers[] = {sys->sampler_linear.get()};
	sys->context->PSSetSamplers(0, 1, samplers);

	// Set blend state
	set_blend_state(sys, data);

	// Draw quad (triangle strip, 4 vertices)
	sys->context->Draw(4, 0);

	// Unbind SRV
	ID3D11ShaderResourceView *null_srv = nullptr;
	sys->context->PSSetShaderResources(0, 1, &null_srv);
}

static void
render_cylinder_layer(struct d3d11_service_system *sys,
                      const struct comp_layer *layer,
                      uint32_t view_index,
                      const struct xrt_pose *view_pose,
                      const struct xrt_fov *fov)
{
	const struct xrt_layer_data *data = &layer->data;
	const struct xrt_layer_cylinder_data *cyl = &data->cylinder;

	// Get swapchain
	struct xrt_swapchain *xsc = layer->sc_array[0];
	if (xsc == nullptr) {
		return;
	}
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain_from_xrt(xsc);

	uint32_t image_index = cyl->sub.image_index;
	if (image_index >= sc->image_count) {
		return;
	}

	ID3D11ShaderResourceView *srv = sc->images[image_index].srv.get();
	if (srv == nullptr) {
		return;
	}

	// Build MVP matrix (cylinder is in layer pose space)
	struct xrt_matrix_4x4 model, view, proj, mv, mvp;

	// Model: just the layer pose (cylinder geometry generated in shader)
	struct xrt_vec3 scale = {1.0f, 1.0f, 1.0f};
	math_matrix_4x4_model(&cyl->pose, &scale, &model);

	// View matrix
	math_matrix_4x4_view_from_pose(view_pose, &view);

	// Projection matrix
	math_matrix_4x4_projection_vulkan_infinite_reverse(fov, 0.1f, &proj);

	// MVP
	math_matrix_4x4_multiply(&view, &model, &mv);
	math_matrix_4x4_multiply(&proj, &mv, &mvp);

	// Fill constant buffer
	CylinderLayerConstants constants = {};
	memcpy(constants.mvp, &mvp, sizeof(constants.mvp));

	// UV transform
	constants.post_transform[0] = cyl->sub.norm_rect.x;
	constants.post_transform[1] = cyl->sub.norm_rect.y;
	constants.post_transform[2] = cyl->sub.norm_rect.w;
	constants.post_transform[3] = cyl->sub.norm_rect.h;

	if (data->flip_y) {
		constants.post_transform[1] += constants.post_transform[3];
		constants.post_transform[3] = -constants.post_transform[3];
	}

	get_color_scale_bias(data, constants.color_scale, constants.color_bias);

	constants.radius = cyl->radius;
	constants.central_angle = cyl->central_angle;
	constants.aspect_ratio = cyl->aspect_ratio;

	// Update constant buffer
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = sys->context->Map(sys->layer_constant_buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (SUCCEEDED(hr)) {
		memcpy(mapped.pData, &constants, sizeof(constants));
		sys->context->Unmap(sys->layer_constant_buffer.get(), 0);
	}

	// Set shaders
	sys->context->VSSetShader(sys->cylinder_vs.get(), nullptr, 0);
	sys->context->PSSetShader(sys->cylinder_ps.get(), nullptr, 0);

	// Bind resources
	ID3D11Buffer *cbs[] = {sys->layer_constant_buffer.get()};
	sys->context->VSSetConstantBuffers(0, 1, cbs);
	sys->context->PSSetConstantBuffers(0, 1, cbs);
	sys->context->PSSetShaderResources(0, 1, &srv);
	ID3D11SamplerState *samplers[] = {sys->sampler_linear.get()};
	sys->context->PSSetSamplers(0, 1, samplers);

	// Set blend state
	set_blend_state(sys, data);

	// Draw cylinder (triangle strip, 2 * (subdivision + 2) vertices)
	// Subdivision count of 64, so 132 vertices
	sys->context->Draw(132, 0);

	// Unbind SRV
	ID3D11ShaderResourceView *null_srv = nullptr;
	sys->context->PSSetShaderResources(0, 1, &null_srv);
}

static void
render_equirect2_layer(struct d3d11_service_system *sys,
                       const struct comp_layer *layer,
                       uint32_t view_index,
                       const struct xrt_pose *view_pose,
                       const struct xrt_fov *fov)
{
	const struct xrt_layer_data *data = &layer->data;
	const struct xrt_layer_equirect2_data *eq = &data->equirect2;

	// Get swapchain
	struct xrt_swapchain *xsc = layer->sc_array[0];
	if (xsc == nullptr) {
		return;
	}
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain_from_xrt(xsc);

	uint32_t image_index = eq->sub.image_index;
	if (image_index >= sc->image_count) {
		return;
	}

	ID3D11ShaderResourceView *srv = sc->images[image_index].srv.get();
	if (srv == nullptr) {
		return;
	}

	// Build inverse model-view matrix (for ray casting)
	struct xrt_matrix_4x4 model, view, mv, mv_inv;

	// Model: layer pose
	struct xrt_vec3 scale = {1.0f, 1.0f, 1.0f};
	math_matrix_4x4_model(&eq->pose, &scale, &model);

	// View matrix
	math_matrix_4x4_view_from_pose(view_pose, &view);

	// MV and inverse
	math_matrix_4x4_multiply(&view, &model, &mv);
	math_matrix_4x4_inverse(&mv, &mv_inv);

	// Calculate UV to tangent transform
	float to_tangent[4];
	to_tangent[0] = tanf(fov->angle_left);
	to_tangent[1] = tanf(fov->angle_down);
	to_tangent[2] = tanf(fov->angle_right) - tanf(fov->angle_left);
	to_tangent[3] = tanf(fov->angle_up) - tanf(fov->angle_down);

	// Fill constant buffer
	Equirect2LayerConstants constants = {};
	memcpy(constants.mv_inverse, &mv_inv, sizeof(constants.mv_inverse));

	// UV transform
	constants.post_transform[0] = eq->sub.norm_rect.x;
	constants.post_transform[1] = eq->sub.norm_rect.y;
	constants.post_transform[2] = eq->sub.norm_rect.w;
	constants.post_transform[3] = eq->sub.norm_rect.h;

	if (data->flip_y) {
		constants.post_transform[1] += constants.post_transform[3];
		constants.post_transform[3] = -constants.post_transform[3];
	}

	get_color_scale_bias(data, constants.color_scale, constants.color_bias);

	memcpy(constants.to_tangent, to_tangent, sizeof(constants.to_tangent));

	// Handle infinite radius (spec says +INFINITY)
	constants.radius = std::isinf(eq->radius) ? 0.0f : eq->radius;
	constants.central_horizontal_angle = eq->central_horizontal_angle;
	constants.upper_vertical_angle = eq->upper_vertical_angle;
	constants.lower_vertical_angle = eq->lower_vertical_angle;

	// Update constant buffer
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = sys->context->Map(sys->layer_constant_buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (SUCCEEDED(hr)) {
		memcpy(mapped.pData, &constants, sizeof(constants));
		sys->context->Unmap(sys->layer_constant_buffer.get(), 0);
	}

	// Set shaders
	sys->context->VSSetShader(sys->equirect2_vs.get(), nullptr, 0);
	sys->context->PSSetShader(sys->equirect2_ps.get(), nullptr, 0);

	// Bind resources
	ID3D11Buffer *cbs[] = {sys->layer_constant_buffer.get()};
	sys->context->VSSetConstantBuffers(0, 1, cbs);
	sys->context->PSSetConstantBuffers(0, 1, cbs);
	sys->context->PSSetShaderResources(0, 1, &srv);
	ID3D11SamplerState *samplers[] = {sys->sampler_linear.get()};
	sys->context->PSSetSamplers(0, 1, samplers);

	// Set blend state
	set_blend_state(sys, data);

	// Draw fullscreen quad
	sys->context->Draw(4, 0);

	// Unbind SRV
	ID3D11ShaderResourceView *null_srv = nullptr;
	sys->context->PSSetShaderResources(0, 1, &null_srv);
}


/*
 *
 * Swapchain functions
 *
 */

static void
swapchain_destroy(struct xrt_swapchain *xsc)
{
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain_from_xrt(xsc);

	// Release all images
	for (uint32_t i = 0; i < sc->image_count; i++) {
		if (sc->images[i].mutex_acquired && sc->images[i].keyed_mutex) {
			sc->images[i].keyed_mutex->ReleaseSync(0);
		}
		sc->images[i].srv.reset();
		sc->images[i].keyed_mutex.reset();
		sc->images[i].texture.reset();
		// Note: Legacy DXGI handles from GetSharedHandle don't need CloseHandle
	}

	delete sc;
}

static xrt_result_t
swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain_from_xrt(xsc);

	// Simple round-robin for now
	static uint32_t next_index = 0;
	*out_index = next_index % sc->image_count;
	next_index++;

	return XRT_SUCCESS;
}

static xrt_result_t
swapchain_inc_image_use(struct xrt_swapchain *xsc, uint32_t index)
{
	// No-op for service compositor
	return XRT_SUCCESS;
}

static xrt_result_t
swapchain_dec_image_use(struct xrt_swapchain *xsc, uint32_t index)
{
	// No-op for service compositor
	return XRT_SUCCESS;
}

static xrt_result_t
swapchain_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain_from_xrt(xsc);

	if (index >= sc->image_count) {
		return XRT_ERROR_NO_IMAGE_AVAILABLE;
	}

	struct d3d11_service_image *img = &sc->images[index];

	if (img->keyed_mutex && !img->mutex_acquired) {
		// Convert timeout to milliseconds
		DWORD timeout_ms = (timeout_ns < 0) ? INFINITE : static_cast<DWORD>(timeout_ns / 1000000);

		HRESULT hr = img->keyed_mutex->AcquireSync(0, timeout_ms);
		if (hr == WAIT_TIMEOUT) {
			return XRT_ERROR_NO_IMAGE_AVAILABLE;
		}
		if (FAILED(hr)) {
			U_LOG_E("KeyedMutex AcquireSync failed: 0x%08lx", hr);
			return XRT_ERROR_NO_IMAGE_AVAILABLE;
		}
		img->mutex_acquired = true;
	}

	return XRT_SUCCESS;
}

static xrt_result_t
swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain_from_xrt(xsc);

	if (index >= sc->image_count) {
		return XRT_ERROR_NO_IMAGE_AVAILABLE;
	}

	struct d3d11_service_image *img = &sc->images[index];

	if (img->keyed_mutex && img->mutex_acquired) {
		img->keyed_mutex->ReleaseSync(0);
		img->mutex_acquired = false;
	}

	return XRT_SUCCESS;
}

static xrt_result_t
swapchain_barrier_image(struct xrt_swapchain *xsc, enum xrt_barrier_direction direction, uint32_t index)
{
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain_from_xrt(xsc);

	if (index >= sc->image_count) {
		return XRT_ERROR_NO_IMAGE_AVAILABLE;
	}

	// D3D11 service compositor: KeyedMutex handles synchronization
	// No additional barrier needed since AcquireSync/ReleaseSync on
	// the KeyedMutex already provides the necessary synchronization
	// between client and service processes.
	(void)direction;

	return XRT_SUCCESS;
}


/*
 *
 * Semaphore functions
 *
 */

static xrt_result_t
semaphore_wait(struct xrt_compositor_semaphore *xcsem, uint64_t value, uint64_t timeout_ns)
{
	struct d3d11_service_semaphore *sem = d3d11_service_semaphore_from_xrt(xcsem);

	// Convert nanoseconds to milliseconds
	auto timeout_ms = std::chrono::milliseconds(timeout_ns / 1000000);

	return xrt::auxiliary::d3d::d3d11::waitOnFenceWithTimeout(
	    sem->fence, sem->wait_event, value, timeout_ms);
}

static void
semaphore_destroy(struct xrt_compositor_semaphore *xcsem)
{
	struct d3d11_service_semaphore *sem = d3d11_service_semaphore_from_xrt(xcsem);

	sem->wait_event.reset();
	sem->fence.reset();
	delete sem;
}


/*
 *
 * Native compositor functions
 *
 */

static xrt_result_t
compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                            const struct xrt_swapchain_create_info *info,
                                            struct xrt_swapchain_create_properties *xsccp)
{
	xsccp->image_count = 3;  // Triple buffering
	xsccp->extra_bits = (enum xrt_swapchain_usage_bits)0;
	return XRT_SUCCESS;
}

/*!
 * Convert XRT format to DXGI format.
 */
static DXGI_FORMAT
xrt_format_to_dxgi(int64_t format)
{
	// Check if this is already a DXGI format (common D3D11 formats are < 130)
	switch (format) {
	// Pass through DXGI formats directly
	case DXGI_FORMAT_R8G8B8A8_UNORM:        // 28
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   // 29
	case DXGI_FORMAT_B8G8R8A8_UNORM:        // 87
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   // 91
	case DXGI_FORMAT_R16G16B16A16_FLOAT:    // 10
	case DXGI_FORMAT_R16G16B16A16_UNORM:    // 11
	case DXGI_FORMAT_D24_UNORM_S8_UINT:     // 45
	case DXGI_FORMAT_D32_FLOAT:             // 40
	case DXGI_FORMAT_D16_UNORM:             // 55
	case DXGI_FORMAT_R10G10B10A2_UNORM:     // 24
		return static_cast<DXGI_FORMAT>(format);

	// Convert VK_FORMAT values to DXGI (for Vulkan compositor interop)
	case 37: // VK_FORMAT_R8G8B8A8_UNORM
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	case 43: // VK_FORMAT_R8G8B8A8_SRGB
		return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case 44: // VK_FORMAT_B8G8R8A8_UNORM
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	case 50: // VK_FORMAT_B8G8R8A8_SRGB
		return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	case 64: // VK_FORMAT_A2B10G10R10_UNORM_PACK32
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	case 97: // VK_FORMAT_R16G16B16A16_SFLOAT
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case 129: // VK_FORMAT_D24_UNORM_S8_UINT
		return DXGI_FORMAT_D24_UNORM_S8_UINT;
	case 130: // VK_FORMAT_D32_SFLOAT
		return DXGI_FORMAT_D32_FLOAT;

	default:
		U_LOG_W("Unknown format %ld, using RGBA8", format);
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	}
}

static xrt_result_t
compositor_create_swapchain(struct xrt_compositor *xc,
                             const struct xrt_swapchain_create_info *info,
                             struct xrt_swapchain **out_xsc)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	struct d3d11_service_system *sys = c->sys;

	// Use triple buffering
	uint32_t image_count = 3;
	if (image_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		image_count = XRT_MAX_SWAPCHAIN_IMAGES;
	}

	U_LOG_W("Creating swapchain: %u images, %ux%u, format=%u, usage=0x%x",
	        image_count, info->width, info->height, info->format, info->bits);

	// Allocate swapchain
	struct d3d11_service_swapchain *sc = new d3d11_service_swapchain();
	std::memset(sc, 0, sizeof(*sc));

	sc->base.base.destroy = swapchain_destroy;
	sc->base.base.acquire_image = swapchain_acquire_image;
	sc->base.base.inc_image_use = swapchain_inc_image_use;
	sc->base.base.dec_image_use = swapchain_dec_image_use;
	sc->base.base.wait_image = swapchain_wait_image;
	sc->base.base.barrier_image = swapchain_barrier_image;
	sc->base.base.release_image = swapchain_release_image;
	sc->base.base.reference.count = 1;
	sc->base.base.image_count = image_count;

	sc->comp = c;
	sc->image_count = image_count;
	sc->info = *info;
	sc->service_created = true; // Created by service for client

	// Convert format
	DXGI_FORMAT dxgi_format = xrt_format_to_dxgi(info->format);

	// Determine bind flags
	UINT bind_flags = D3D11_BIND_SHADER_RESOURCE; // Always need SRV for compositor
	if (info->bits & XRT_SWAPCHAIN_USAGE_COLOR) {
		bind_flags |= D3D11_BIND_RENDER_TARGET;
	}
	if (info->bits & XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL) {
		bind_flags |= D3D11_BIND_DEPTH_STENCIL;
	}
	if (info->bits & XRT_SWAPCHAIN_USAGE_UNORDERED_ACCESS) {
		bind_flags |= D3D11_BIND_UNORDERED_ACCESS;
	}

	// Create texture descriptor with SHARED_KEYEDMUTEX for cross-process sharing
	D3D11_TEXTURE2D_DESC tex_desc = {};
	tex_desc.Width = info->width;
	tex_desc.Height = info->height;
	tex_desc.MipLevels = info->mip_count > 0 ? info->mip_count : 1;
	tex_desc.ArraySize = info->array_size > 0 ? info->array_size : 1;
	tex_desc.Format = dxgi_format;
	tex_desc.SampleDesc.Count = info->sample_count > 0 ? info->sample_count : 1;
	tex_desc.SampleDesc.Quality = 0;
	tex_desc.Usage = D3D11_USAGE_DEFAULT;
	tex_desc.BindFlags = bind_flags;
	tex_desc.CPUAccessFlags = 0;
	// SHARED_KEYEDMUTEX enables cross-process sharing with synchronization
	tex_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

	// Create textures and get shared handles
	for (uint32_t i = 0; i < image_count; i++) {
		HRESULT hr = sys->device->CreateTexture2D(&tex_desc, nullptr, sc->images[i].texture.put());
		if (FAILED(hr)) {
			U_LOG_E("Failed to create shared texture [%u]: 0x%08lx", i, hr);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}

		// Get KeyedMutex for synchronization
		hr = sc->images[i].texture->QueryInterface(IID_PPV_ARGS(sc->images[i].keyed_mutex.put()));
		if (FAILED(hr)) {
			U_LOG_E("Texture has no KeyedMutex [%u]: 0x%08lx", i, hr);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}

		// Get DXGI shared handle for IPC transfer to client (matches SRHydra approach)
		wil::com_ptr<IDXGIResource> dxgi_resource;
		hr = sc->images[i].texture->QueryInterface(IID_PPV_ARGS(dxgi_resource.put()));
		if (FAILED(hr)) {
			U_LOG_E("Failed to get IDXGIResource [%u]: 0x%08lx", i, hr);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}

		HANDLE shared_handle = nullptr;
		hr = dxgi_resource->GetSharedHandle(&shared_handle);
		if (FAILED(hr) || shared_handle == nullptr) {
			U_LOG_E("Failed to get shared handle [%u]: 0x%08lx", i, hr);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}

		// Store shared handle for IPC layer - DO NOT set bit 0
		// This allows IPC to properly DuplicateHandle to the client process (like SRHydra)
		sc->base.images[i].handle = (xrt_graphics_buffer_handle_t)shared_handle;
		sc->base.images[i].size = 0; // Unknown for D3D11
		sc->base.images[i].use_dedicated_allocation = false;
		sc->base.images[i].is_dxgi_handle = true;

		U_LOG_W("Created shared texture [%u]: handle=%p (DXGI, will DuplicateHandle)", i, shared_handle);

		// Create SRV for compositor
		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = dxgi_format;
		srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MipLevels = tex_desc.MipLevels;

		hr = sys->device->CreateShaderResourceView(
		    sc->images[i].texture.get(), &srv_desc, sc->images[i].srv.put());
		if (FAILED(hr)) {
			U_LOG_E("Failed to create SRV [%u]: 0x%08lx", i, hr);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}
	}

	U_LOG_W("Created swapchain with %u shared images (%ux%u, format=%d)",
	        image_count, info->width, info->height, (int)dxgi_format);

	*out_xsc = &sc->base.base;
	return XRT_SUCCESS;
}

static xrt_result_t
compositor_import_swapchain(struct xrt_compositor *xc,
                             const struct xrt_swapchain_create_info *info,
                             struct xrt_image_native *native_images,
                             uint32_t image_count,
                             struct xrt_swapchain **out_xsc)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	struct d3d11_service_system *sys = c->sys;

	if (image_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		U_LOG_E("Too many images: %u > %u", image_count, XRT_MAX_SWAPCHAIN_IMAGES);
		return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
	}

	// Allocate swapchain
	struct d3d11_service_swapchain *sc = new d3d11_service_swapchain();
	std::memset(sc, 0, sizeof(*sc));

	sc->base.base.destroy = swapchain_destroy;
	sc->base.base.acquire_image = swapchain_acquire_image;
	sc->base.base.inc_image_use = swapchain_inc_image_use;
	sc->base.base.dec_image_use = swapchain_dec_image_use;
	sc->base.base.wait_image = swapchain_wait_image;
	sc->base.base.barrier_image = swapchain_barrier_image;
	sc->base.base.release_image = swapchain_release_image;
	sc->base.base.reference.count = 1;

	sc->comp = c;
	sc->image_count = image_count;
	sc->info = *info;
	sc->service_created = false; // Imported from client

	U_LOG_W("Importing swapchain: %u images, %ux%u, format=%u, usage=0x%x",
	        image_count, info->width, info->height, info->format, info->bits);

	// Import each image from the client
	for (uint32_t i = 0; i < image_count; i++) {
		HANDLE handle = native_images[i].handle;

		if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
			U_LOG_E("Invalid handle for image [%u]: %p", i, handle);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}

		// Check for DXGI handle encoding (bit 0 set)
		bool is_dxgi = native_images[i].is_dxgi_handle;
		if ((size_t)handle & 1) {
			handle = (HANDLE)((size_t)handle - 1);
			is_dxgi = true;
		}

		U_LOG_D("Image [%u]: handle=%p, is_dxgi=%d", i, handle, is_dxgi);

		// Open shared resource
		// is_dxgi = true means a legacy DXGI global handle (from IDXGIResource::GetSharedHandle,
		// created without NTHANDLE flag). These are system-wide and don't need DuplicateHandle.
		// is_dxgi = false means an NT handle (from IDXGIResource1::CreateSharedHandle,
		// created with NTHANDLE flag). These require DuplicateHandle across processes.
		HRESULT hr;
		if (is_dxgi) {
			// Legacy DXGI global handle → use OpenSharedResource (ID3D11Device)
			hr = sys->device->OpenSharedResource(handle, IID_PPV_ARGS(sc->images[i].texture.put()));
			if (FAILED(hr)) {
				U_LOG_E("OpenSharedResource (DXGI global handle) failed for image [%u]: 0x%08lx (handle=%p)",
				        i, hr, handle);
			}
		} else {
			// NT handle → use OpenSharedResource1 (ID3D11Device1)
			hr = sys->device->OpenSharedResource1(handle, IID_PPV_ARGS(sc->images[i].texture.put()));
			if (FAILED(hr)) {
				U_LOG_E("OpenSharedResource1 (NT handle) failed for image [%u]: 0x%08lx (handle=%p)",
				        i, hr, handle);
			}
		}

		if (FAILED(hr)) {
			// Log additional diagnostic information
			U_LOG_E("  Swapchain info: %ux%u, format=%u", info->width, info->height, info->format);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}

		U_LOG_W("Image [%u] imported successfully (%s handle)", i, is_dxgi ? "DXGI global" : "NT");

		// Get KeyedMutex for synchronization
		hr = sc->images[i].texture->QueryInterface(IID_PPV_ARGS(sc->images[i].keyed_mutex.put()));
		if (FAILED(hr)) {
			U_LOG_W("Shared texture has no KeyedMutex, synchronization may be unreliable");
		}

		// Create shader resource view
		D3D11_TEXTURE2D_DESC desc;
		sc->images[i].texture->GetDesc(&desc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = desc.Format;
		srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MipLevels = 1;

		hr = sys->device->CreateShaderResourceView(
		    sc->images[i].texture.get(), &srv_desc, sc->images[i].srv.put());

		if (FAILED(hr)) {
			U_LOG_E("Failed to create SRV [%u]: 0x%08lx", i, hr);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}
	}

	U_LOG_W("Imported swapchain with %u images (%ux%u)", image_count, info->width, info->height);

	sc->base.base.image_count = image_count;
	*out_xsc = &sc->base.base;
	return XRT_SUCCESS;
}

static xrt_result_t
compositor_import_fence(struct xrt_compositor *xc,
                         xrt_graphics_sync_handle_t handle,
                         struct xrt_compositor_fence **out_xcf)
{
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
compositor_create_semaphore(struct xrt_compositor *xc,
                             xrt_graphics_sync_handle_t *out_handle,
                             struct xrt_compositor_semaphore **out_xcsem)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	struct d3d11_service_system *sys = c->sys;

	// Allocate semaphore
	struct d3d11_service_semaphore *sem = new d3d11_service_semaphore();
	std::memset(&sem->base, 0, sizeof(sem->base));

	sem->sys = sys;
	sem->base.reference.count = 1;
	sem->base.wait = semaphore_wait;
	sem->base.destroy = semaphore_destroy;

	// Create the wait event
	sem->wait_event.create();
	if (!sem->wait_event) {
		U_LOG_E("Failed to create wait event for semaphore");
		delete sem;
		return XRT_ERROR_FENCE_CREATE_FAILED;
	}

	// Create a shared D3D11 fence
	xrt_graphics_sync_handle_t handle = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
	xrt_result_t xret = xrt::auxiliary::d3d::d3d11::createSharedFence(
	    *sys->device.get(),
	    false,  // share_cross_adapter
	    &handle,
	    sem->fence);

	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to create D3D11 fence for semaphore");
		delete sem;
		return XRT_ERROR_FENCE_CREATE_FAILED;
	}

	U_LOG_I("Created D3D11 compositor semaphore");

	*out_handle = handle;
	*out_xcsem = &sem->base;

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	U_LOG_W("D3D11 service compositor: session begin");
	return XRT_SUCCESS;
}

static xrt_result_t
compositor_end_session(struct xrt_compositor *xc)
{
	U_LOG_W("D3D11 service compositor: session end");
	return XRT_SUCCESS;
}

static xrt_result_t
compositor_predict_frame(struct xrt_compositor *xc,
                          int64_t *out_frame_id,
                          int64_t *out_wake_time_ns,
                          int64_t *out_predicted_gpu_time_ns,
                          int64_t *out_predicted_display_time_ns,
                          int64_t *out_predicted_display_period_ns)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	c->frame_id++;
	*out_frame_id = c->frame_id;

	int64_t now_ns = static_cast<int64_t>(os_monotonic_get_ns());
	int64_t period_ns = static_cast<int64_t>(U_TIME_1S_IN_NS / c->sys->refresh_rate);

	*out_predicted_display_time_ns = now_ns + period_ns * 2;
	*out_predicted_display_period_ns = period_ns;
	*out_wake_time_ns = now_ns;
	*out_predicted_gpu_time_ns = period_ns;

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_wait_frame(struct xrt_compositor *xc,
                       int64_t *out_frame_id,
                       int64_t *out_predicted_display_time_ns,
                       int64_t *out_predicted_display_period_ns)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	c->frame_id++;
	*out_frame_id = c->frame_id;

	int64_t now_ns = static_cast<int64_t>(os_monotonic_get_ns());
	int64_t period_ns = static_cast<int64_t>(U_TIME_1S_IN_NS / c->sys->refresh_rate);

	*out_predicted_display_time_ns = now_ns + period_ns * 2;
	*out_predicted_display_period_ns = period_ns;

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_mark_frame(struct xrt_compositor *xc,
                       int64_t frame_id,
                       enum xrt_compositor_frame_point point,
                       int64_t when_ns)
{
	return XRT_SUCCESS;
}

static xrt_result_t
compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	c->layer_accum.layer_count = 0;

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	c->layer_accum.layer_count = 0;

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_begin(&c->layer_accum, data);

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_projection(struct xrt_compositor *xc,
                             struct xrt_device *xdev,
                             struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                             const struct xrt_layer_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_projection(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_projection_depth(struct xrt_compositor *xc,
                                   struct xrt_device *xdev,
                                   struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                   struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                   const struct xrt_layer_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_projection_depth(&c->layer_accum, xsc, d_xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_quad(struct xrt_compositor *xc,
                       struct xrt_device *xdev,
                       struct xrt_swapchain *xsc,
                       const struct xrt_layer_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_quad(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_cube(struct xrt_compositor *xc,
                       struct xrt_device *xdev,
                       struct xrt_swapchain *xsc,
                       const struct xrt_layer_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_cube(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_cylinder(struct xrt_compositor *xc,
                           struct xrt_device *xdev,
                           struct xrt_swapchain *xsc,
                           const struct xrt_layer_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_cylinder(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_equirect1(struct xrt_compositor *xc,
                            struct xrt_device *xdev,
                            struct xrt_swapchain *xsc,
                            const struct xrt_layer_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_equirect1(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_equirect2(struct xrt_compositor *xc,
                            struct xrt_device *xdev,
                            struct xrt_swapchain *xsc,
                            const struct xrt_layer_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_equirect2(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_passthrough(struct xrt_compositor *xc,
                              struct xrt_device *xdev,
                              const struct xrt_layer_data *data)
{
	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	struct d3d11_service_system *sys = c->sys;

	std::lock_guard<std::mutex> lock(c->mutex);

	// Log frame submission (first frame and every 60 frames)
	static uint32_t frame_count = 0;
	if (frame_count == 0 || frame_count % 60 == 0) {
		U_LOG_W("layer_commit: frame %u, layers=%u", frame_count, c->layer_accum.layer_count);
	}
	frame_count++;

	// Clear stereo render target
	if (sys->stereo_rtv) {
		float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		sys->context->ClearRenderTargetView(sys->stereo_rtv.get(), clear_color);
	}

	// Render projection layers to stereo texture (via copy)
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];

		if (layer->data.type != XRT_LAYER_PROJECTION &&
		    layer->data.type != XRT_LAYER_PROJECTION_DEPTH) {
			continue;
		}

		// Get left and right swapchains
		struct xrt_swapchain *xsc_left = layer->sc_array[0];
		struct xrt_swapchain *xsc_right = layer->sc_array[1];

		if (xsc_left == nullptr || xsc_right == nullptr) {
			U_LOG_W("Projection layer missing swapchain");
			continue;
		}

		struct d3d11_service_swapchain *sc_left = d3d11_service_swapchain_from_xrt(xsc_left);
		struct d3d11_service_swapchain *sc_right = d3d11_service_swapchain_from_xrt(xsc_right);

		// Get image indices from layer data
		uint32_t left_index = layer->data.proj.v[0].sub.image_index;
		uint32_t right_index = layer->data.proj.v[1].sub.image_index;

		if (left_index >= sc_left->image_count || right_index >= sc_right->image_count) {
			U_LOG_W("Invalid image index in projection layer");
			continue;
		}

		ID3D11Texture2D *left_tex = sc_left->images[left_index].texture.get();
		ID3D11Texture2D *right_tex = sc_right->images[right_index].texture.get();

		if (left_tex == nullptr || right_tex == nullptr) {
			U_LOG_W("Missing texture in projection layer");
			continue;
		}

		// Copy left view to left half of stereo texture
		D3D11_BOX left_box = {};
		left_box.left = layer->data.proj.v[0].sub.rect.offset.w;
		left_box.top = layer->data.proj.v[0].sub.rect.offset.h;
		left_box.right = left_box.left + layer->data.proj.v[0].sub.rect.extent.w;
		left_box.bottom = left_box.top + layer->data.proj.v[0].sub.rect.extent.h;
		left_box.front = 0;
		left_box.back = 1;

		sys->context->CopySubresourceRegion(
		    sys->stereo_texture.get(),
		    0,             // dst subresource
		    0, 0, 0,       // dst x, y, z (left half)
		    left_tex,
		    layer->data.proj.v[0].sub.array_index,  // src subresource
		    &left_box);

		// Copy right view to right half of stereo texture
		D3D11_BOX right_box = {};
		right_box.left = layer->data.proj.v[1].sub.rect.offset.w;
		right_box.top = layer->data.proj.v[1].sub.rect.offset.h;
		right_box.right = right_box.left + layer->data.proj.v[1].sub.rect.extent.w;
		right_box.bottom = right_box.top + layer->data.proj.v[1].sub.rect.extent.h;
		right_box.front = 0;
		right_box.back = 1;

		sys->context->CopySubresourceRegion(
		    sys->stereo_texture.get(),
		    0,                            // dst subresource
		    sys->view_width, 0, 0,        // dst x, y, z (right half)
		    right_tex,
		    layer->data.proj.v[1].sub.array_index,  // src subresource
		    &right_box);

		U_LOG_T("Rendered projection layer %u", i);
	}

	// Check if there are any UI layers to render
	bool has_ui_layers = false;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];
		switch (layer->data.type) {
		case XRT_LAYER_QUAD:
		case XRT_LAYER_CYLINDER:
		case XRT_LAYER_EQUIRECT2:
		case XRT_LAYER_EQUIRECT1:
		case XRT_LAYER_CUBE:
			has_ui_layers = true;
			break;
		default:
			break;
		}
		if (has_ui_layers) break;
	}

	// Render UI layers if any exist and shaders are ready
	if (has_ui_layers && sys->quad_vs) {
		// Bind stereo render target
		ID3D11RenderTargetView *rtvs[] = {sys->stereo_rtv.get()};
		sys->context->OMSetRenderTargets(1, rtvs, nullptr);

		// Set common rendering state
		sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		sys->context->IASetInputLayout(nullptr);  // Using SV_VertexID
		sys->context->RSSetState(sys->rasterizer_state.get());
		sys->context->OMSetDepthStencilState(sys->depth_disabled.get(), 0);

		// Create default view poses and FOVs for each eye
		// Note: In a full implementation, these would come from tracking
		struct xrt_pose view_poses[2];
		struct xrt_fov fovs[2];

		// Default identity poses
		view_poses[0].orientation.x = 0.0f;
		view_poses[0].orientation.y = 0.0f;
		view_poses[0].orientation.z = 0.0f;
		view_poses[0].orientation.w = 1.0f;
		view_poses[0].position.x = -0.032f;  // IPD/2
		view_poses[0].position.y = 0.0f;
		view_poses[0].position.z = 0.0f;

		view_poses[1].orientation.x = 0.0f;
		view_poses[1].orientation.y = 0.0f;
		view_poses[1].orientation.z = 0.0f;
		view_poses[1].orientation.w = 1.0f;
		view_poses[1].position.x = 0.032f;  // IPD/2
		view_poses[1].position.y = 0.0f;
		view_poses[1].position.z = 0.0f;

		// Default symmetric FOV (roughly 90 degrees)
		const float fov_angle = 0.785f;  // ~45 degrees
		for (uint32_t view = 0; view < 2; view++) {
			fovs[view].angle_left = -fov_angle;
			fovs[view].angle_right = fov_angle;
			fovs[view].angle_up = fov_angle;
			fovs[view].angle_down = -fov_angle;
		}

		// Render UI layers for each eye
		for (uint32_t view_index = 0; view_index < 2; view_index++) {
			// Set viewport for this eye
			D3D11_VIEWPORT viewport = {};
			viewport.TopLeftX = static_cast<float>(view_index * sys->view_width);
			viewport.TopLeftY = 0.0f;
			viewport.Width = static_cast<float>(sys->view_width);
			viewport.Height = static_cast<float>(sys->view_height);
			viewport.MinDepth = 0.0f;
			viewport.MaxDepth = 1.0f;
			sys->context->RSSetViewports(1, &viewport);

			// Render equirect2 layers first (background/skybox)
			for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
				struct comp_layer *layer = &c->layer_accum.layers[i];
				if (layer->data.type == XRT_LAYER_EQUIRECT2) {
					if (is_layer_view_visible(&layer->data, view_index)) {
						render_equirect2_layer(sys, layer, view_index,
						                       &view_poses[view_index],
						                       &fovs[view_index]);
					}
				}
			}

			// Render cylinder layers
			for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
				struct comp_layer *layer = &c->layer_accum.layers[i];
				if (layer->data.type == XRT_LAYER_CYLINDER) {
					if (is_layer_view_visible(&layer->data, view_index)) {
						render_cylinder_layer(sys, layer, view_index,
						                      &view_poses[view_index],
						                      &fovs[view_index]);
					}
				}
			}

			// Render quad layers last (on top)
			for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
				struct comp_layer *layer = &c->layer_accum.layers[i];
				if (layer->data.type == XRT_LAYER_QUAD) {
					if (is_layer_view_visible(&layer->data, view_index)) {
						render_quad_layer(sys, layer, view_index,
						                  &view_poses[view_index],
						                  &fovs[view_index]);
					}
				}
			}
		}
	}

#ifdef XRT_HAVE_LEIA_SR_D3D11
	// Weave stereo texture through SR weaver for light field display
	if (sys->weaver != nullptr && sys->stereo_srv) {
		// Set input stereo texture
		leiasr_d3d11_set_input_texture(
		    sys->weaver,
		    sys->stereo_srv.get(),
		    sys->view_width,
		    sys->view_height,
		    DXGI_FORMAT_R8G8B8A8_UNORM);

		// Bind back buffer as output
		ID3D11RenderTargetView *rtvs[] = {sys->back_buffer_rtv.get()};
		sys->context->OMSetRenderTargets(1, rtvs, nullptr);

		// Set viewport (native display resolution for weaver output)
		D3D11_VIEWPORT viewport = {};
		viewport.Width = static_cast<float>(sys->output_width);
		viewport.Height = static_cast<float>(sys->output_height);
		viewport.MaxDepth = 1.0f;
		sys->context->RSSetViewports(1, &viewport);

		// Perform weaving
		leiasr_d3d11_weave(sys->weaver);
	} else
#endif
	{
		// No weaver - copy stereo texture to back buffer directly
		if (sys->stereo_texture && sys->back_buffer_rtv) {
			wil::com_ptr<ID3D11Resource> back_buffer;
			sys->back_buffer_rtv->GetResource(back_buffer.put());
			sys->context->CopyResource(back_buffer.get(), sys->stereo_texture.get());
		}
	}

	// Present to display
	if (sys->swap_chain) {
		sys->swap_chain->Present(1, 0);  // VSync
	}

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_commit_with_semaphore(struct xrt_compositor *xc,
                                        struct xrt_compositor_semaphore *xcsem,
                                        uint64_t value)
{
	return compositor_layer_commit(xc, XRT_GRAPHICS_SYNC_HANDLE_INVALID);
}

static void
compositor_destroy(struct xrt_compositor *xc)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	delete c;
}


/*
 *
 * System compositor functions
 *
 */

static xrt_result_t
system_create_native_compositor(struct xrt_system_compositor *xsysc,
                                const struct xrt_session_info *xsi,
                                struct xrt_session_event_sink *xses,
                                struct xrt_compositor_native **out_xcn)
{
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);

	// Create per-client native compositor
	struct d3d11_service_compositor *c = new d3d11_service_compositor();
	std::memset(&c->base, 0, sizeof(c->base));

	c->sys = sys;
	c->log_level = sys->log_level;
	c->frame_id = 0;

	// Initialize layer accumulator
	std::memset(&c->layer_accum, 0, sizeof(c->layer_accum));

	// Set up compositor vtable
	c->base.base.get_swapchain_create_properties = compositor_get_swapchain_create_properties;
	c->base.base.create_swapchain = compositor_create_swapchain;
	c->base.base.import_swapchain = compositor_import_swapchain;
	c->base.base.import_fence = compositor_import_fence;
	c->base.base.create_semaphore = compositor_create_semaphore;
	c->base.base.begin_session = compositor_begin_session;
	c->base.base.end_session = compositor_end_session;
	c->base.base.predict_frame = compositor_predict_frame;
	c->base.base.wait_frame = compositor_wait_frame;
	c->base.base.mark_frame = compositor_mark_frame;
	c->base.base.begin_frame = compositor_begin_frame;
	c->base.base.discard_frame = compositor_discard_frame;
	c->base.base.layer_begin = compositor_layer_begin;
	c->base.base.layer_projection = compositor_layer_projection;
	c->base.base.layer_projection_depth = compositor_layer_projection_depth;
	c->base.base.layer_quad = compositor_layer_quad;
	c->base.base.layer_cube = compositor_layer_cube;
	c->base.base.layer_cylinder = compositor_layer_cylinder;
	c->base.base.layer_equirect1 = compositor_layer_equirect1;
	c->base.base.layer_equirect2 = compositor_layer_equirect2;
	c->base.base.layer_passthrough = compositor_layer_passthrough;
	c->base.base.layer_commit = compositor_layer_commit;
	c->base.base.layer_commit_with_semaphore = compositor_layer_commit_with_semaphore;
	c->base.base.destroy = compositor_destroy;

	// Set up supported formats
	// IMPORTANT: Store as VkFormat values, not DXGI! The IPC protocol and D3D11 client
	// compositor expect VkFormat values which they then convert to DXGI.
	// The d3d_dxgi_format_to_vk() function converts DXGI -> VkFormat.
	uint32_t format_count = 0;
	c->base.base.info.formats[format_count++] = d3d_dxgi_format_to_vk(DXGI_FORMAT_R8G8B8A8_UNORM);
	c->base.base.info.formats[format_count++] = d3d_dxgi_format_to_vk(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	c->base.base.info.formats[format_count++] = d3d_dxgi_format_to_vk(DXGI_FORMAT_B8G8R8A8_UNORM);
	c->base.base.info.formats[format_count++] = d3d_dxgi_format_to_vk(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
	c->base.base.info.formats[format_count++] = d3d_dxgi_format_to_vk(DXGI_FORMAT_R16G16B16A16_FLOAT);
	c->base.base.info.format_count = format_count;

	// Log the formats being reported
	U_LOG_W("D3D11 service compositor: reporting %u VkFormat values to IPC client:", format_count);
	for (uint32_t i = 0; i < format_count; i++) {
		U_LOG_W("  format[%u] = %lld (VkFormat)", i, (long long)c->base.base.info.formats[i]);
	}

	U_LOG_W("D3D11 service: created native compositor for client");

	*out_xcn = &c->base;
	return XRT_SUCCESS;
}

static void
system_destroy(struct xrt_system_compositor *xsysc)
{
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);

	U_LOG_I("Destroying D3D11 service system compositor");

#ifdef XRT_HAVE_LEIA_SR_D3D11
	if (sys->weaver != nullptr) {
		leiasr_d3d11_destroy(&sys->weaver);
	}
#endif

	// Clean up layer rendering resources
	sys->depth_disabled.reset();
	sys->rasterizer_state.reset();
	sys->blend_opaque.reset();
	sys->blend_premul.reset();
	sys->blend_alpha.reset();
	sys->sampler_linear.reset();
	sys->layer_constant_buffer.reset();

	// Clean up layer shaders
	sys->cube_ps.reset();
	sys->cube_vs.reset();
	sys->equirect2_ps.reset();
	sys->equirect2_vs.reset();
	sys->cylinder_ps.reset();
	sys->cylinder_vs.reset();
	sys->quad_ps.reset();
	sys->quad_vs.reset();

	sys->back_buffer_rtv.reset();
	sys->stereo_rtv.reset();
	sys->stereo_srv.reset();
	sys->stereo_texture.reset();
	sys->swap_chain.reset();
	sys->dxgi_factory.reset();
	sys->context.reset();
	sys->device.reset();

	if (sys->window != nullptr) {
		comp_d3d11_window_destroy(&sys->window);
	}
	sys->hwnd = nullptr;

	delete sys;
}


/*
 *
 * Exported functions
 *
 */

extern "C" xrt_result_t
comp_d3d11_service_create_system(struct xrt_device *xdev,
                                 struct xrt_system_compositor **out_xsysc)
{
	U_LOG_W("Creating D3D11 service system compositor");

	// Allocate system compositor
	struct d3d11_service_system *sys = new d3d11_service_system();
	std::memset(&sys->base, 0, sizeof(sys->base));

	sys->xdev = xdev;
	sys->log_level = U_LOGGING_INFO;

	// Query SR display for recommended view dimensions, native resolution, and refresh rate
	// - View dimensions: per-eye texture size for quality rendering (reported to apps)
	// - Native dimensions: physical display resolution (for window/swap chain output)
	uint32_t sr_view_width = 0;
	uint32_t sr_view_height = 0;
	uint32_t sr_native_width = 0;
	uint32_t sr_native_height = 0;
	float sr_refresh_rate = 0.0f;

#ifdef XRT_HAVE_LEIA_SR_D3D11
	if (leiasr_query_recommended_view_dimensions(5.0, &sr_view_width, &sr_view_height, &sr_refresh_rate,
	                                              &sr_native_width, &sr_native_height)) {
		U_LOG_W("Using SR recommended view dimensions: %ux%u per eye, %.0f Hz",
		        sr_view_width, sr_view_height, sr_refresh_rate);
		U_LOG_W("Using SR native display dimensions: %ux%u for window/swap chain",
		        sr_native_width, sr_native_height);

		// Per-eye view dimensions (reported to apps for swapchain creation)
		sys->view_width = sr_view_width;
		sys->view_height = sr_view_height;

		// Stereo texture dimensions (side-by-side input to weaver)
		sys->display_width = sys->view_width * 2;
		sys->display_height = sys->view_height;

		// Output dimensions (window/swap chain at native display resolution)
		sys->output_width = sr_native_width;
		sys->output_height = sr_native_height;

		sys->refresh_rate = sr_refresh_rate > 0.0f ? sr_refresh_rate : 60.0f;
	} else {
		U_LOG_W("Could not query SR display dimensions, using defaults");
		sys->display_width = 1920;
		sys->display_height = 1080;
		sys->output_width = 2560;   // Default Leia display native resolution
		sys->output_height = 1440;
		sys->view_width = sys->display_width / 2;
		sys->view_height = sys->display_height;
		sys->refresh_rate = 60.0f;
	}
#else
	sys->display_width = 1920;
	sys->display_height = 1080;
	sys->output_width = sys->display_width;
	sys->output_height = sys->display_height;
	sys->view_width = sys->display_width / 2;
	sys->view_height = sys->display_height;
	sys->refresh_rate = 60.0f;
#endif

	// Create D3D11 device (service owns this, independent of clients)
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
	flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	D3D_FEATURE_LEVEL actual_level;

	wil::com_ptr<ID3D11Device> device_base;
	wil::com_ptr<ID3D11DeviceContext> context_base;

	HRESULT hr = D3D11CreateDevice(
	    nullptr,                     // Default adapter
	    D3D_DRIVER_TYPE_HARDWARE,
	    nullptr,
	    flags,
	    feature_levels, ARRAYSIZE(feature_levels),
	    D3D11_SDK_VERSION,
	    device_base.put(),
	    &actual_level,
	    context_base.put());

	if (FAILED(hr)) {
		U_LOG_E("Failed to create D3D11 device: 0x%08lx", hr);
		delete sys;
		return XRT_ERROR_VULKAN;  // Generic graphics error
	}

	// Get ID3D11Device5 and ID3D11DeviceContext4 for shared resource support
	if (!device_base.try_query_to(sys->device.put())) {
		U_LOG_E("Device doesn't support ID3D11Device5");
		delete sys;
		return XRT_ERROR_VULKAN;
	}

	if (!context_base.try_query_to(sys->context.put())) {
		U_LOG_E("Context doesn't support ID3D11DeviceContext4");
		delete sys;
		return XRT_ERROR_VULKAN;
	}

	// Get DXGI factory
	wil::com_ptr<IDXGIDevice> dxgi_device;
	sys->device.try_query_to(dxgi_device.put());

	wil::com_ptr<IDXGIAdapter> adapter;
	dxgi_device->GetAdapter(adapter.put());

	hr = adapter->GetParent(IID_PPV_ARGS(sys->dxgi_factory.put()));
	if (FAILED(hr)) {
		U_LOG_E("Failed to get DXGI factory: 0x%08lx", hr);
		delete sys;
		return XRT_ERROR_VULKAN;
	}

	// Create service window on dedicated thread (handles message pump)
	// Window is created at native display resolution for proper fullscreen
	xrt_result_t wret = comp_d3d11_window_create(sys->output_width, sys->output_height, &sys->window);
	if (wret != XRT_SUCCESS || sys->window == NULL) {
		U_LOG_E("Failed to create service window on dedicated thread");
		delete sys;
		return XRT_ERROR_VULKAN;
	}
	sys->hwnd = (HWND)comp_d3d11_window_get_hwnd(sys->window);

	// Create swap chain at native display resolution (output of weaver)
	DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
	sc_desc.Width = sys->output_width;
	sc_desc.Height = sys->output_height;
	sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sc_desc.SampleDesc.Count = 1;
	sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sc_desc.BufferCount = 2;
	sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

	hr = sys->dxgi_factory->CreateSwapChainForHwnd(
	    sys->device.get(),
	    sys->hwnd,
	    &sc_desc,
	    nullptr,
	    nullptr,
	    sys->swap_chain.put());

	if (FAILED(hr)) {
		U_LOG_E("Failed to create swap chain: 0x%08lx", hr);
		system_destroy(&sys->base);
		return XRT_ERROR_VULKAN;
	}

	// Get back buffer RTV
	wil::com_ptr<ID3D11Texture2D> back_buffer;
	sys->swap_chain->GetBuffer(0, IID_PPV_ARGS(back_buffer.put()));
	sys->device->CreateRenderTargetView(back_buffer.get(), nullptr, sys->back_buffer_rtv.put());

	// Create stereo render target texture (side-by-side views)
	D3D11_TEXTURE2D_DESC stereo_desc = {};
	stereo_desc.Width = sys->display_width;
	stereo_desc.Height = sys->display_height;
	stereo_desc.MipLevels = 1;
	stereo_desc.ArraySize = 1;
	stereo_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	stereo_desc.SampleDesc.Count = 1;
	stereo_desc.Usage = D3D11_USAGE_DEFAULT;
	stereo_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

	hr = sys->device->CreateTexture2D(&stereo_desc, nullptr, sys->stereo_texture.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create stereo texture: 0x%08lx", hr);
		system_destroy(&sys->base);
		return XRT_ERROR_VULKAN;
	}

	// Create SRV for stereo texture
	hr = sys->device->CreateShaderResourceView(sys->stereo_texture.get(), nullptr, sys->stereo_srv.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create stereo SRV: 0x%08lx", hr);
		system_destroy(&sys->base);
		return XRT_ERROR_VULKAN;
	}

	// Create RTV for stereo texture
	hr = sys->device->CreateRenderTargetView(sys->stereo_texture.get(), nullptr, sys->stereo_rtv.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create stereo RTV: 0x%08lx", hr);
		system_destroy(&sys->base);
		return XRT_ERROR_VULKAN;
	}

	U_LOG_W("Created stereo render target (%ux%u)", sys->display_width, sys->display_height);

	// Create layer shaders and resources for UI layer rendering
	if (!create_layer_shaders(sys)) {
		U_LOG_W("Failed to create layer shaders, UI layers will not render");
		// Don't fail - projection layers will still work
	} else if (!create_layer_resources(sys)) {
		U_LOG_W("Failed to create layer resources, UI layers will not render");
		// Don't fail - projection layers will still work
	}

#ifdef XRT_HAVE_LEIA_SR_D3D11
	// Create SR weaver
	{
		xrt_result_t weaver_ret = leiasr_d3d11_create(
		    5.0,  // 5 second timeout
		    sys->device.get(),
		    sys->context.get(),
		    sys->hwnd,
		    sys->view_width,
		    sys->view_height,
		    &sys->weaver);

		if (weaver_ret != XRT_SUCCESS) {
			U_LOG_W("Failed to create SR weaver, continuing without interlacing");
			sys->weaver = nullptr;
		}
	}
#endif

	// Set up system compositor vtable
	sys->base.create_native_compositor = system_create_native_compositor;
	sys->base.destroy = system_destroy;

	// Fill system compositor info
	sys->base.info.max_layers = XRT_MAX_LAYERS;
	sys->base.info.views[0].recommended.width_pixels = sys->view_width;
	sys->base.info.views[0].recommended.height_pixels = sys->view_height;
	sys->base.info.views[0].max.width_pixels = sys->view_width;
	sys->base.info.views[0].max.height_pixels = sys->view_height;
	sys->base.info.views[1] = sys->base.info.views[0];

	U_LOG_W("D3D11 service system compositor created: view=%ux%u/eye, stereo=%ux%u, output=%ux%u @ %.0fHz",
	        sys->view_width, sys->view_height,
	        sys->display_width, sys->display_height,
	        sys->output_width, sys->output_height, sys->refresh_rate);

	*out_xsysc = &sys->base;
	return XRT_SUCCESS;
}
