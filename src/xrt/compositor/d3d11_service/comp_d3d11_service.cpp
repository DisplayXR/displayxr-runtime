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
#include "xrt/xrt_limits.h"
#include "xrt/xrt_session.h"
#include "xrt/xrt_display_processor_d3d11.h"
#include "xrt/xrt_display_metrics.h"

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

#include "util/u_hud.h"
#include "util/u_tiling.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
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
#include <sddl.h>


/*
 *
 * Helpers
 *
 */

// Helper to create security attributes for AppContainer sharing
static bool
create_appcontainer_sa(SECURITY_ATTRIBUTES &sa, PSECURITY_DESCRIPTOR &sd)
{
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = FALSE;

	// D: DACL
	// (A;;GA;;;AC) Allow Generic All to AppContainer (S-1-15-2-1)
	// (A;;GA;;;WD) Allow Generic All to Everyone (S-1-1-0) - for safety/debugging
	// (A;;GA;;;BA) Allow Generic All to Built-in Admins
	// (A;;GA;;;IU) Allow Generic All to Interactive User
	const wchar_t *sddl = L"D:(A;;GA;;;AC)(A;;GA;;;WD)(A;;GA;;;BA)(A;;GA;;;IU)";

	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
	        sddl, SDDL_REVISION_1, &sd, NULL)) {
		U_LOG_E("ConvertStringSecurityDescriptorToSecurityDescriptorW failed: %lu", GetLastError());
		return false;
	}

	sa.lpSecurityDescriptor = sd;
	return true;
}



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
 * Per-client render resources.
 *
 * These resources are created when a client connects and destroyed when the
 * client disconnects. This allows multiple clients to have their own windows
 * and display processors, and allows the IPC service to start without creating a
 * window until a client actually connects.
 */
struct d3d11_client_render_resources
{
	//! Dedicated-thread window (NULL if using external HWND)
	struct comp_d3d11_window *window;

	//! HWND for swap chain and display processor (owned or external)
	HWND hwnd;

	//! Whether we own the window (created it) or it's external
	bool owns_window;

	//! DXGI swap chain for display output
	wil::com_ptr<IDXGISwapChain1> swap_chain;

	//! Back buffer render target view
	wil::com_ptr<ID3D11RenderTargetView> back_buffer_rtv;

	//! Atlas render target (tiled views)
	wil::com_ptr<ID3D11Texture2D> atlas_texture;
	wil::com_ptr<ID3D11ShaderResourceView> atlas_srv;
	wil::com_ptr<ID3D11RenderTargetView> atlas_rtv;

	//! Generic D3D11 display processor (vendor-agnostic weaving)
	struct xrt_display_processor_d3d11 *display_processor;

	//! HUD overlay (runtime-owned windows only)
	struct u_hud *hud;

	//! D3D11 staging texture for HUD pixel upload
	wil::com_ptr<ID3D11Texture2D> hud_texture;

	//! True if HUD GPU resources are initialized
	bool hud_initialized;

	//! Last frame timestamp for FPS calculation
	uint64_t last_frame_time_ns;

	//! Smoothed frame time for display
	float smoothed_frame_time_ms;
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

	//! Session event sink for pushing state change events
	struct xrt_session_event_sink *xses;

	//! Current visibility state
	bool state_visible;

	//! Current focus state
	bool state_focused;

	//! Whether the window has been closed (triggers session exit)
	bool window_closed;

	//! Whether the EXIT_REQUEST event has already been sent (prevent duplicates)
	bool exit_request_sent;

	//! Number of frames since window close was detected
	uint32_t window_closed_frame_count;

	//! Per-client render resources (window, swap chain, display processor)
	struct d3d11_client_render_resources render;

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
 *
 * Contains shared resources used by all clients (D3D11 device, shaders, etc.)
 * Per-client resources (window, swap chain, display processor) are in d3d11_client_render_resources.
 */
struct d3d11_service_system
{
	//! Base system compositor - must be first!
	struct xrt_system_compositor base;

	//! Multi-compositor control interface for session state management
	struct xrt_multi_compositor_control xmcc;

	//! The device we are rendering for
	struct xrt_device *xdev;

	//! System devices for qwerty input support (passed to per-client windows)
	struct xrt_system_devices *xsysd;

	//! D3D11 device (owned by service, not the app)
	wil::com_ptr<ID3D11Device5> device;

	//! D3D11 immediate context
	wil::com_ptr<ID3D11DeviceContext4> context;

	//! DXGI factory
	wil::com_ptr<IDXGIFactory4> dxgi_factory;

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

	//! Blit shaders for projection layer copy with SRGB conversion
	wil::com_ptr<ID3D11VertexShader> blit_vs;
	wil::com_ptr<ID3D11PixelShader> blit_ps;
	wil::com_ptr<ID3D11Buffer> blit_constant_buffer;

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

	//! Atlas texture dimensions (tiled views, input to display processor)
	uint32_t display_width;
	uint32_t display_height;

	//! Output dimensions (window/swap chain, native display resolution)
	uint32_t output_width;
	uint32_t output_height;

	//! View dimensions (per eye, reported to apps)
	uint32_t view_width;
	uint32_t view_height;

	//! Tile layout for atlas (from active rendering mode, default 2x1)
	uint32_t tile_columns;
	uint32_t tile_rows;

	//! Display refresh rate
	float refresh_rate;

	//! Logging level
	enum u_logging_level log_level;

	//! Active compositor (for eye position queries)
	//! Points to the most recently active client's compositor.
	//! Set during layer_commit, cleared on compositor destroy.
	struct d3d11_service_compositor *active_compositor;

	//! Mutex for active_compositor access
	std::mutex active_compositor_mutex;

	//! True when display is in 3D mode (weaver active). False = 2D passthrough.
	bool hardware_display_3d;

	//! Last known 3D rendering mode index (for V-key toggle restore).
	uint32_t last_3d_mode_index;
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

/*!
 * Sync tile layout from the active rendering mode of the head device.
 * Defaults to 2 columns, 1 row (side-by-side stereo) if not available.
 */
static void
sync_tile_layout(struct d3d11_service_system *sys)
{
	sys->tile_columns = 2;
	sys->tile_rows = 1;

	if (sys->xdev != NULL && sys->xdev->hmd != NULL) {
		uint32_t idx = sys->xdev->hmd->active_rendering_mode_index;
		if (idx < sys->xdev->rendering_mode_count) {
			uint32_t tc = sys->xdev->rendering_modes[idx].tile_columns;
			uint32_t tr = sys->xdev->rendering_modes[idx].tile_rows;
			if (tc > 0 && tr > 0) {
				sys->tile_columns = tc;
				sys->tile_rows = tr;
			}
		}
	}
}

/*!
 * Check if a DXGI format is an SRGB format.
 */
static inline bool
is_srgb_format(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
	case DXGI_FORMAT_BC2_UNORM_SRGB:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
		return true;
	default:
		return false;
	}
}

/*!
 * Get the SRGB variant of a format for SRV creation.
 * Returns the same format if already SRGB or no SRGB variant exists.
 */
static inline DXGI_FORMAT
get_srgb_format(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
		return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	case DXGI_FORMAT_B8G8R8X8_UNORM:
		return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
	default:
		return format;  // Return as-is
	}
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

	// Blit vertex shader (for projection layer copy with SRGB conversion)
	hr = compile_shader(blit_vs_hlsl, "VSMain", "vs_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile blit vertex shader");
		return false;
	}
	hr = sys->device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                      sys->blit_vs.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create blit vertex shader: 0x%08lx", hr);
		return false;
	}

	// Blit pixel shader
	hr = compile_shader(blit_ps_hlsl, "PSMain", "ps_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile blit pixel shader");
		return false;
	}
	hr = sys->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                     sys->blit_ps.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create blit pixel shader: 0x%08lx", hr);
		return false;
	}

	U_LOG_I("Created all layer shaders (including blit)");
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

	// Create blit constant buffer
	D3D11_BUFFER_DESC blit_cb_desc = {};
	blit_cb_desc.ByteWidth = static_cast<UINT>((sizeof(BlitConstants) + 15) & ~15);
	blit_cb_desc.Usage = D3D11_USAGE_DYNAMIC;
	blit_cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	blit_cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	hr = sys->device->CreateBuffer(&blit_cb_desc, nullptr, sys->blit_constant_buffer.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create blit constant buffer: 0x%08lx", hr);
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
 * Projection layer blit with SRGB conversion
 *
 */

/*!
 * Blit a region from source texture to stereo texture with optional SRGB conversion.
 *
 * This replaces CopySubresourceRegion when the source is SRGB, ensuring proper
 * gamma handling. When source is SRGB, the GPU linearizes on sample and we
 * re-encode to sRGB for the display processor.
 *
 * @param sys The system compositor
 * @param src_tex Source texture to blit from
 * @param src_srv SRV for the source texture (should be SRGB format if source is SRGB)
 * @param src_rect Source rectangle (x, y, width, height) in pixels
 * @param src_size Source texture size (width, height)
 * @param dst_x Destination X offset in stereo texture
 * @param dst_y Destination Y offset in stereo texture
 * @param is_srgb Whether source is SRGB format (triggers gamma conversion)
 */
static void
blit_to_atlas_texture(struct d3d11_service_system *sys,
                       struct d3d11_client_render_resources *res,
                       ID3D11ShaderResourceView *src_srv,
                       float src_x, float src_y, float src_w, float src_h,
                       float src_tex_w, float src_tex_h,
                       float dst_x, float dst_y,
                       bool is_srgb)
{
	// Update blit constant buffer
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = sys->context->Map(sys->blit_constant_buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) {
		U_LOG_E("Failed to map blit constant buffer: 0x%08lx", hr);
		return;
	}

	BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
	cb->src_rect[0] = src_x;
	cb->src_rect[1] = src_y;
	cb->src_rect[2] = src_w;
	cb->src_rect[3] = src_h;
	cb->dst_offset[0] = dst_x;
	cb->dst_offset[1] = dst_y;
	cb->src_size[0] = src_tex_w;
	cb->src_size[1] = src_tex_h;
	cb->dst_size[0] = static_cast<float>(sys->display_width);
	cb->dst_size[1] = static_cast<float>(sys->display_height);
	cb->convert_srgb = is_srgb ? 1.0f : 0.0f;
	cb->padding = 0.0f;

	sys->context->Unmap(sys->blit_constant_buffer.get(), 0);

	// Set up pipeline for blit
	sys->context->VSSetShader(sys->blit_vs.get(), nullptr, 0);
	sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0);
	sys->context->VSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
	sys->context->PSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
	sys->context->PSSetShaderResources(0, 1, &src_srv);
	sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());

	// Set render target to per-client stereo texture
	ID3D11RenderTargetView *rtvs[] = {res->atlas_rtv.get()};
	sys->context->OMSetRenderTargets(1, rtvs, nullptr);

	// Set viewport to cover destination region
	D3D11_VIEWPORT vp = {};
	vp.TopLeftX = 0;
	vp.TopLeftY = 0;
	vp.Width = static_cast<float>(sys->display_width);
	vp.Height = static_cast<float>(sys->display_height);
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	sys->context->RSSetViewports(1, &vp);

	// Set blend state to opaque (overwrite)
	float blend_factor[4] = {0, 0, 0, 0};
	sys->context->OMSetBlendState(sys->blend_opaque.get(), blend_factor, 0xFFFFFFFF);
	sys->context->OMSetDepthStencilState(sys->depth_disabled.get(), 0);
	sys->context->RSSetState(sys->rasterizer_state.get());

	// Draw fullscreen quad (4 vertices, triangle strip)
	sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	sys->context->IASetInputLayout(nullptr);
	sys->context->Draw(4, 0);

	// Clear shader resources to avoid hazards
	ID3D11ShaderResourceView *null_srv = nullptr;
	sys->context->PSSetShaderResources(0, 1, &null_srv);
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
 * Per-client render resource management
 *
 */

/*!
 * Clean up per-client render resources.
 */
static void
fini_client_render_resources(struct d3d11_client_render_resources *res)
{
	if (res == nullptr) {
		return;
	}

	// Clean up HUD resources
	res->hud_texture.reset();
	u_hud_destroy(&res->hud);

	// Auto-switch to 2D mode before destroying display processor
	if (res->display_processor != nullptr) {
		xrt_display_processor_d3d11_request_display_mode(
		    res->display_processor, false);
	}
	xrt_display_processor_d3d11_destroy(&res->display_processor);

	res->back_buffer_rtv.reset();
	res->atlas_rtv.reset();
	res->atlas_srv.reset();
	res->atlas_texture.reset();
	res->swap_chain.reset();

	if (res->owns_window && res->window != nullptr) {
		comp_d3d11_window_destroy(&res->window);
	}
	res->window = nullptr;
	res->hwnd = nullptr;
	res->owns_window = false;
}

/*!
 * Initialize per-client render resources.
 *
 * @param sys The system compositor (provides device, dimensions)
 * @param external_hwnd External window handle from XR_EXT_win32_window_binding, or NULL
 * @param xsysd System devices for qwerty input (may be NULL)
 * @param res Output render resources struct
 * @return XRT_SUCCESS on success
 */
static xrt_result_t
init_client_render_resources(struct d3d11_service_system *sys,
                              void *external_hwnd,
                              struct xrt_system_devices *xsysd,
                              struct d3d11_client_render_resources *res)
{
	std::memset(res, 0, sizeof(*res));

	HRESULT hr;

	// Get or create window
	if (external_hwnd != nullptr) {
		// Use app-provided window (XR_EXT_win32_window_binding)
		res->hwnd = (HWND)external_hwnd;
		res->owns_window = false;
		res->window = nullptr;
		U_LOG_W("Using external window handle: %p", external_hwnd);
	} else {
		// Create our own window (IPC/WebXR path)
		xrt_result_t wret = comp_d3d11_window_create(sys->output_width, sys->output_height, &res->window);
		if (wret != XRT_SUCCESS || res->window == nullptr) {
			U_LOG_E("Failed to create window for client");
			return XRT_ERROR_VULKAN;
		}
		res->hwnd = (HWND)comp_d3d11_window_get_hwnd(res->window);
		res->owns_window = true;

		// Pass system devices to window for qwerty input support
		if (xsysd != nullptr) {
			comp_d3d11_window_set_system_devices(res->window, xsysd);
			U_LOG_W("Passed xsysd to client window for qwerty input");
		}

		U_LOG_W("Created window for client: hwnd=%p (%ux%u)", res->hwnd, sys->output_width, sys->output_height);
	}

	// Get actual window client area (may differ from requested size if window
	// went fullscreen to native monitor resolution during creation)
	uint32_t actual_width = sys->output_width;
	uint32_t actual_height = sys->output_height;
	if (res->hwnd != nullptr) {
		RECT client_rect;
		if (GetClientRect(res->hwnd, &client_rect)) {
			uint32_t cw = static_cast<uint32_t>(client_rect.right - client_rect.left);
			uint32_t ch = static_cast<uint32_t>(client_rect.bottom - client_rect.top);
			if (cw > 0 && ch > 0) {
				actual_width = cw;
				actual_height = ch;
				if (cw != sys->output_width || ch != sys->output_height) {
					U_LOG_W("Window actual size differs from defaults: %ux%u (was %ux%u)",
					        cw, ch, sys->output_width, sys->output_height);
				}
			}
		}
	}

	// Create HUD overlay for runtime-owned windows
	if (res->owns_window) {
		res->smoothed_frame_time_ms = 16.67f;
		u_hud_create(&res->hud, actual_width);
	}

	// Create swap chain at actual window size (not defaults)
	DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
	sc_desc.Width = actual_width;
	sc_desc.Height = actual_height;
	sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sc_desc.SampleDesc.Count = 1;
	sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sc_desc.BufferCount = 2;
	sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

	hr = sys->dxgi_factory->CreateSwapChainForHwnd(
	    sys->device.get(),
	    res->hwnd,
	    &sc_desc,
	    nullptr,
	    nullptr,
	    res->swap_chain.put());

	if (FAILED(hr)) {
		U_LOG_E("Failed to create swap chain for client: 0x%08lx", hr);
		fini_client_render_resources(res);
		return XRT_ERROR_VULKAN;
	}

	// Get back buffer RTV
	wil::com_ptr<ID3D11Texture2D> back_buffer;
	res->swap_chain->GetBuffer(0, IID_PPV_ARGS(back_buffer.put()));
	sys->device->CreateRenderTargetView(back_buffer.get(), nullptr, res->back_buffer_rtv.put());

	// Create stereo render target texture (side-by-side views)
	D3D11_TEXTURE2D_DESC atlas_desc = {};
	atlas_desc.Width = sys->display_width;
	atlas_desc.Height = sys->display_height;
	atlas_desc.MipLevels = 1;
	atlas_desc.ArraySize = 1;
	atlas_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	atlas_desc.SampleDesc.Count = 1;
	atlas_desc.Usage = D3D11_USAGE_DEFAULT;
	atlas_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

	hr = sys->device->CreateTexture2D(&atlas_desc, nullptr, res->atlas_texture.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create atlas texture for client: 0x%08lx", hr);
		fini_client_render_resources(res);
		return XRT_ERROR_VULKAN;
	}

	// Create SRV for stereo texture
	hr = sys->device->CreateShaderResourceView(res->atlas_texture.get(), nullptr, res->atlas_srv.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create atlas SRV for client: 0x%08lx", hr);
		fini_client_render_resources(res);
		return XRT_ERROR_VULKAN;
	}

	// Create RTV for stereo texture
	hr = sys->device->CreateRenderTargetView(res->atlas_texture.get(), nullptr, res->atlas_rtv.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create atlas RTV for client: 0x%08lx", hr);
		fini_client_render_resources(res);
		return XRT_ERROR_VULKAN;
	}

	U_LOG_W("Created stereo render target for client (%ux%u)", sys->display_width, sys->display_height);

	// Create display processor via factory (set by the target builder at init time)
	if (sys->base.info.dp_factory_d3d11 != NULL) {
		auto factory = (xrt_dp_factory_d3d11_fn_t)sys->base.info.dp_factory_d3d11;
		xrt_result_t dp_ret = factory(sys->device.get(), sys->context.get(), res->hwnd, &res->display_processor);
		if (dp_ret != XRT_SUCCESS) {
			U_LOG_W("D3D11 display processor factory failed (error %d), continuing without",
			        (int)dp_ret);
			res->display_processor = nullptr;
		} else {
			U_LOG_W("D3D11 display processor created via factory for client");
			// Auto-switch to 3D mode when display processor is ready
			if (sys->hardware_display_3d) {
				xrt_display_processor_d3d11_request_display_mode(
				    res->display_processor, true);
			}

			// Query display pixel info from the real (windowed) display processor.
			// The temp DP at system init uses NULL window and may fail to return
			// pixel info, leaving sys dimensions at 1920x1080 defaults.  Update now.
			uint32_t dp_px_w = 0, dp_px_h = 0;
			int32_t dp_left = 0, dp_top = 0;
			if (xrt_display_processor_d3d11_get_display_pixel_info(
			        res->display_processor, &dp_px_w, &dp_px_h,
			        &dp_left, &dp_top) &&
			    dp_px_w > 0 && dp_px_h > 0 &&
			    (dp_px_w != sys->output_width || dp_px_h != sys->output_height)) {
				U_LOG_W("Updating dims from display processor: %ux%u -> %ux%u",
				        sys->output_width, sys->output_height, dp_px_w, dp_px_h);
				sync_tile_layout(sys);
				sys->output_width = dp_px_w;
				sys->output_height = dp_px_h;
				sys->view_width = dp_px_w / sys->tile_columns;
				sys->view_height = dp_px_h / sys->tile_rows;
				sys->display_width = sys->tile_columns * sys->view_width;
				sys->display_height = sys->tile_rows * sys->view_height;

				// Recreate stereo texture at correct dimensions
				res->atlas_rtv.reset();
				res->atlas_srv.reset();
				res->atlas_texture.reset();

				D3D11_TEXTURE2D_DESC atlas_desc = {};
				atlas_desc.Width = sys->display_width;
				atlas_desc.Height = sys->display_height;
				atlas_desc.MipLevels = 1;
				atlas_desc.ArraySize = 1;
				atlas_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				atlas_desc.SampleDesc.Count = 1;
				atlas_desc.Usage = D3D11_USAGE_DEFAULT;
				atlas_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

				hr = sys->device->CreateTexture2D(&atlas_desc, nullptr,
				                                  res->atlas_texture.put());
				if (SUCCEEDED(hr)) {
					sys->device->CreateShaderResourceView(
					    res->atlas_texture.get(), nullptr, res->atlas_srv.put());
					sys->device->CreateRenderTargetView(
					    res->atlas_texture.get(), nullptr, res->atlas_rtv.put());
					U_LOG_W("Stereo texture recreated at %ux%u",
					        sys->display_width, sys->display_height);
				}

				// Resize window and swap chain to match display
				if (res->owns_window && res->hwnd != nullptr) {
					RECT client_rect;
					if (GetClientRect(res->hwnd, &client_rect)) {
						uint32_t cw = (uint32_t)(client_rect.right - client_rect.left);
						uint32_t ch = (uint32_t)(client_rect.bottom - client_rect.top);
						if (cw != dp_px_w || ch != dp_px_h) {
							// Resize swap chain to match
							res->back_buffer_rtv.reset();
							HRESULT rhr = res->swap_chain->ResizeBuffers(
							    0, dp_px_w, dp_px_h, DXGI_FORMAT_UNKNOWN, 0);
							if (SUCCEEDED(rhr)) {
								wil::com_ptr<ID3D11Texture2D> bb;
								res->swap_chain->GetBuffer(0, IID_PPV_ARGS(bb.put()));
								sys->device->CreateRenderTargetView(
								    bb.get(), nullptr, res->back_buffer_rtv.put());
								U_LOG_W("Swap chain resized to %ux%u to match display",
								        dp_px_w, dp_px_h);
							}
						}
					}
				}
			}
		}
	} else {
		U_LOG_W("No D3D11 display processor factory provided");
		res->display_processor = nullptr;
	}

	U_LOG_W("Client render resources initialized: view=%ux%u/eye, stereo=%ux%u, output=%ux%u",
	        sys->view_width, sys->view_height,
	        sys->display_width, sys->display_height,
	        sys->output_width, sys->output_height);

	return XRT_SUCCESS;
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

		// Close NT handles for service-created swapchains
		if (sc->service_created && sc->base.images[i].handle != nullptr) {
			CloseHandle((HANDLE)sc->base.images[i].handle);
			sc->base.images[i].handle = nullptr;
		}
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

	// For server-created swapchains (WebXR), the CLIENT owns the KeyedMutex
	// during wait_image/release_image. The client handles mutex synchronization
	// directly in comp_d3d11_client.cpp. Server only acquires mutex later when
	// it needs to read the texture for composition (in layer_commit).
	if (sc->service_created) {
		// Server-created: client handles mutex, just return success
		return XRT_SUCCESS;
	}

	// For client-created swapchains (imported), server acquires mutex here
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

	// For server-created swapchains (WebXR), the CLIENT owns the KeyedMutex
	// during wait_image/release_image. The client handles mutex release
	// directly in comp_d3d11_client.cpp.
	if (sc->service_created) {
		// Server-created: client handles mutex, just return success
		return XRT_SUCCESS;
	}

	// For client-created swapchains (imported), server releases mutex here
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
	xsccp->image_count = 1;  // Single buffer like SR Hydra for WebXR compatibility
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

	// Use single buffer like SR Hydra for WebXR compatibility
	uint32_t image_count = 1;
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
	// SHARED_NTHANDLE creates real kernel handles that can be DuplicateHandle'd to Chrome
	tex_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

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

		// Get NT handle via IDXGIResource1::CreateSharedHandle
		// NT handles are real kernel handles that can be DuplicateHandle'd to Chrome's process
		wil::com_ptr<IDXGIResource1> dxgi_resource1;
		hr = sc->images[i].texture->QueryInterface(IID_PPV_ARGS(dxgi_resource1.put()));
		if (FAILED(hr)) {
			U_LOG_E("Failed to get IDXGIResource1 [%u]: 0x%08lx", i, hr);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}

		// Create security attributes for AppContainer sharing
		SECURITY_ATTRIBUTES sa = {};
		PSECURITY_DESCRIPTOR sd = nullptr;
		create_appcontainer_sa(sa, sd);

		HANDLE shared_handle = nullptr;
		hr = dxgi_resource1->CreateSharedHandle(
		    &sa, // AppContainer security
		    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
		    nullptr, // no name
		    &shared_handle);

		if (sd) LocalFree(sd);
		if (FAILED(hr) || shared_handle == nullptr) {
			U_LOG_E("Failed to create NT shared handle [%u]: 0x%08lx", i, hr);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}

		// Store NT handle - DO NOT set bit 0, allowing IPC to DuplicateHandle to client
		sc->base.images[i].handle = (xrt_graphics_buffer_handle_t)shared_handle;
		sc->base.images[i].size = 0; // Unknown for D3D11
		sc->base.images[i].use_dedicated_allocation = false;
		sc->base.images[i].is_dxgi_handle = false; // NT handle, use OpenSharedResource1

		U_LOG_W("Created shared texture [%u]: handle=%p (NT handle)", i, shared_handle);

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

	// Note: KeyedMutex starts in released state (key 0), so client can acquire immediately.
	// No initial release needed.
	for (uint32_t i = 0; i < image_count; i++) {
		sc->images[i].mutex_acquired = false;
	}

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

	// Create security attributes for AppContainer sharing
	SECURITY_ATTRIBUTES sa = {};
	PSECURITY_DESCRIPTOR sd = nullptr;
	create_appcontainer_sa(sa, sd);

	// Create a shared D3D11 fence
	xrt_graphics_sync_handle_t handle = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
	xrt_result_t xret = xrt::auxiliary::d3d::d3d11::createSharedFence(
	    *sys->device.get(),
	    false,  // share_cross_adapter
	    &handle,
	    sem->fence,
	    &sa);

	if (sd) LocalFree(sd);

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

	// Failsafe: if window closed and client keeps calling predict_frame
	// without layer_commit, push exit request after a few frames
	if (c->window_closed && c->window_closed_frame_count >= 3) {
		if (!c->exit_request_sent && c->xses != nullptr) {
			U_LOG_W("Window closed failsafe: %u frames since close, requesting session exit",
			        c->window_closed_frame_count);
			union xrt_session_event xse = XRT_STRUCT_INIT;
			xse.type = XRT_SESSION_EVENT_EXIT_REQUEST;
			xrt_session_event_sink_push(c->xses, &xse);
			c->exit_request_sent = true;
		}
	}

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

	// If window was closed, push exit request and return dummy frame data
	if (c->window_closed) {
		if (!c->exit_request_sent && c->xses != nullptr) {
			union xrt_session_event xse = XRT_STRUCT_INIT;
			xse.type = XRT_SESSION_EVENT_EXIT_REQUEST;
			xrt_session_event_sink_push(c->xses, &xse);
			c->exit_request_sent = true;
		}
		c->frame_id++;
		*out_frame_id = c->frame_id;
		int64_t now_ns = static_cast<int64_t>(os_monotonic_get_ns());
		int64_t period_ns = static_cast<int64_t>(U_TIME_1S_IN_NS / c->sys->refresh_rate);
		*out_predicted_display_time_ns = now_ns + period_ns * 2;
		*out_predicted_display_period_ns = period_ns;
		return XRT_SUCCESS;
	}

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

/*!
 * Render the HUD overlay onto the back buffer (post-weave).
 * Uses CopySubresourceRegion for zero-shader simplicity.
 */
static void
d3d11_service_render_hud(struct d3d11_service_system *sys,
                          struct d3d11_client_render_resources *res,
                          bool weaving_done,
                          const struct xrt_eye_positions *eye_pos)
{
	if (!res->owns_window || res->hud == NULL || !u_hud_is_visible()) {
		return;
	}

	// Compute FPS from frame timestamps
	uint64_t now_ns = os_monotonic_get_ns();
	if (res->last_frame_time_ns != 0) {
		float dt_ms = (float)(now_ns - res->last_frame_time_ns) / 1e6f;
		// Exponential moving average (alpha=0.1 for smooth display)
		res->smoothed_frame_time_ms = res->smoothed_frame_time_ms * 0.9f + dt_ms * 0.1f;
	}
	res->last_frame_time_ns = now_ns;

	float fps = (res->smoothed_frame_time_ms > 0.0f) ? (1000.0f / res->smoothed_frame_time_ms) : 0.0f;

	// Get render and window dimensions
	uint32_t render_w = sys->view_width;
	uint32_t render_h = sys->view_height;
	uint32_t win_w = sys->output_width;
	uint32_t win_h = sys->output_height;
	if (res->hwnd != nullptr) {
		RECT rc;
		if (GetClientRect(res->hwnd, &rc)) {
			uint32_t ww = (uint32_t)(rc.right - rc.left);
			uint32_t wh = (uint32_t)(rc.bottom - rc.top);
			if (ww > 0 && wh > 0) {
				win_w = ww;
				win_h = wh;
			}
		}
	}

	// Get display physical dimensions from display processor
	float disp_w_mm = 0.0f, disp_h_mm = 0.0f;
	float nom_x = 0.0f, nom_y = 0.0f, nom_z = 600.0f;
	{
		float w_m = 0.0f, h_m = 0.0f;
		if (xrt_display_processor_d3d11_get_display_dimensions(
		        res->display_processor, &w_m, &h_m)) {
			disp_w_mm = w_m * 1000.0f;
			disp_h_mm = h_m * 1000.0f;
		}
	}

	// Fill HUD data
	struct u_hud_data data = {};
	data.device_name = sys->xdev->str;
	data.fps = fps;
	data.frame_time_ms = res->smoothed_frame_time_ms;
	data.mode_3d = sys->hardware_display_3d;
	if (sys->xdev != NULL && sys->xdev->hmd != NULL) {
		uint32_t idx = sys->xdev->hmd->active_rendering_mode_index;
		if (idx < sys->xdev->rendering_mode_count) {
			data.rendering_mode_name = sys->xdev->rendering_modes[idx].mode_name;
		}
	}
	data.render_width = render_w;
	data.render_height = render_h;
	if (sys->xdev != NULL && sys->xdev->rendering_mode_count > 0) {
		u_tiling_compute_system_atlas(sys->xdev->rendering_modes, sys->xdev->rendering_mode_count,
		                              &data.swapchain_width, &data.swapchain_height);
	}
	data.window_width = win_w;
	data.window_height = win_h;
	data.display_width_mm = disp_w_mm;
	data.display_height_mm = disp_h_mm;
	data.nominal_x = nom_x;
	data.nominal_y = nom_y;
	data.nominal_z = nom_z;
	data.eye_count = eye_pos->count;
	for (uint32_t e = 0; e < eye_pos->count && e < 8; e++) {
		data.eyes[e].x = eye_pos->eyes[e].x * 1000.0f;
		data.eyes[e].y = eye_pos->eyes[e].y * 1000.0f;
		data.eyes[e].z = eye_pos->eyes[e].z * 1000.0f;
	}
	data.eye_tracking_active = eye_pos->is_tracking;

#ifdef XRT_BUILD_DRIVER_QWERTY
	if (sys->xsysd != nullptr) {
		// Virtual display position + forward vector from qwerty device pose.
		struct xrt_pose qwerty_pose;
		if (qwerty_get_hmd_pose(sys->xsysd->xdevs, sys->xsysd->xdev_count, &qwerty_pose)) {
			data.vdisp_x = qwerty_pose.position.x;
			data.vdisp_y = qwerty_pose.position.y;
			data.vdisp_z = qwerty_pose.position.z;
			struct xrt_vec3 fwd_in = {0, 0, -1};
			struct xrt_vec3 fwd_out;
			math_quat_rotate_vec3(&qwerty_pose.orientation, &fwd_in, &fwd_out);
			data.forward_x = fwd_out.x;
			data.forward_y = fwd_out.y;
			data.forward_z = fwd_out.z;
		}

		struct qwerty_view_state ss;
		if (qwerty_get_view_state(sys->xsysd->xdevs, sys->xsysd->xdev_count, &ss)) {
			data.camera_mode = ss.camera_mode;
			data.cam_spread_factor = ss.cam_spread_factor;
			data.cam_parallax_factor = ss.cam_parallax_factor;
			data.cam_convergence = ss.cam_convergence;
			data.cam_half_tan_vfov = ss.cam_half_tan_vfov;
			data.disp_spread_factor = ss.disp_spread_factor;
			data.disp_parallax_factor = ss.disp_parallax_factor;
			data.disp_vHeight = ss.disp_vHeight;
			data.nominal_viewer_z = ss.nominal_viewer_z;
			data.screen_height_m = ss.screen_height_m;
		}
	}
#endif

	bool dirty = u_hud_update(res->hud, &data);

	// Lazy-create the D3D11 staging texture
	if (!res->hud_initialized) {
		uint32_t hud_w = u_hud_get_width(res->hud);
		uint32_t hud_h = u_hud_get_height(res->hud);

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = hud_w;
		desc.Height = hud_h;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = 0; // No shader binding needed, just copy source

		HRESULT hr = sys->device->CreateTexture2D(&desc, nullptr, res->hud_texture.put());
		if (FAILED(hr)) {
			U_LOG_E("Failed to create HUD texture: 0x%08lx", hr);
			return;
		}
		res->hud_initialized = true;
		dirty = true; // Force initial upload
	}

	// Upload pixels to staging texture if changed
	if (dirty && res->hud_texture) {
		uint32_t hud_w = u_hud_get_width(res->hud);
		sys->context->UpdateSubresource(res->hud_texture.get(), 0, nullptr,
		                                 u_hud_get_pixels(res->hud),
		                                 hud_w * 4, 0);
	}

	// Blit HUD texture to bottom-left of back buffer
	if (res->hud_texture && res->swap_chain) {
		wil::com_ptr<ID3D11Texture2D> back_buffer;
		res->swap_chain->GetBuffer(0, IID_PPV_ARGS(back_buffer.put()));
		if (back_buffer) {
			uint32_t hud_w = u_hud_get_width(res->hud);
			uint32_t hud_h = u_hud_get_height(res->hud);

			// Position at bottom-left with 10px margin
			uint32_t dst_x = 10;
			uint32_t dst_y = (win_h > hud_h + 10) ? (win_h - hud_h - 10) : 0;

			D3D11_BOX src_box = {0, 0, 0, hud_w, hud_h, 1};
			sys->context->CopySubresourceRegion(back_buffer.get(), 0, dst_x, dst_y, 0,
			                                     res->hud_texture.get(), 0, &src_box);
		}
	}
}

static xrt_result_t
compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	struct d3d11_service_system *sys = c->sys;

	std::lock_guard<std::mutex> lock(c->mutex);

	// Check window validity - detect window close to end session
	if (!c->window_closed) {
		bool window_valid = true;
		if (c->render.owns_window && c->render.window != nullptr) {
			window_valid = comp_d3d11_window_is_valid(c->render.window);
		} else if (c->render.hwnd != nullptr) {
			window_valid = IsWindow(c->render.hwnd) != FALSE;
		}
		if (!window_valid) {
			U_LOG_W("Window closed - requesting session exit");
			c->window_closed = true;
			c->exit_request_sent = false;
			c->window_closed_frame_count = 0;
		}
	}

	if (c->window_closed) {
		c->window_closed_frame_count++;
		// Push EXIT_REQUEST once to trigger graceful session shutdown
		if (!c->exit_request_sent && c->xses != nullptr) {
			union xrt_session_event xse = XRT_STRUCT_INIT;
			xse.type = XRT_SESSION_EVENT_EXIT_REQUEST;
			xrt_session_event_sink_push(c->xses, &xse);
			c->exit_request_sent = true;
		}
		// Return success so the error doesn't propagate as XR_ERROR_INSTANCE_LOST.
		// The EXIT_REQUEST event drives the session to STOPPING so the app
		// calls xrEndSession and continues running.
		return XRT_SUCCESS;
	}

	// Track this as the active compositor for eye position queries
	{
		std::lock_guard<std::mutex> active_lock(sys->active_compositor_mutex);
		sys->active_compositor = c;
	}

	// Log frame submission (first frame and every 60 frames)
	static uint32_t frame_count = 0;
	if (frame_count == 0 || frame_count % 60 == 0) {
		U_LOG_W("layer_commit: frame %u, layers=%u", frame_count, c->layer_accum.layer_count);
	}
	frame_count++;

	// Handle window resize - check if swap chain needs to be resized
	// This is critical for SR weaving which requires viewport to match window
	// Check if in drag mode - defer expensive stereo texture reallocation during drag
	bool in_size_move = false;
	if (c->render.owns_window && c->render.window != nullptr) {
		in_size_move = comp_d3d11_window_is_in_size_move(c->render.window);
	}

	if (c->render.hwnd != nullptr && c->render.swap_chain) {
		RECT client_rect;
		if (GetClientRect(c->render.hwnd, &client_rect)) {
			uint32_t client_width = static_cast<uint32_t>(client_rect.right - client_rect.left);
			uint32_t client_height = static_cast<uint32_t>(client_rect.bottom - client_rect.top);

			// Check if swap chain size matches window client area
			DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
			c->render.swap_chain->GetDesc1(&sc_desc);

			if (client_width > 0 && client_height > 0 &&
			    (sc_desc.Width != client_width || sc_desc.Height != client_height)) {
				U_LOG_W("Window resize detected: swap_chain=%ux%u, client=%ux%u - resizing%s",
				        sc_desc.Width, sc_desc.Height, client_width, client_height,
				        in_size_move ? " (drag in progress, deferring stereo resize)" : "");

				// Release back buffer RTV before resize
				c->render.back_buffer_rtv.reset();

				// Resize swap chain buffers - always do this immediately (DXGI requirement)
				HRESULT hr = c->render.swap_chain->ResizeBuffers(
				    0,  // Keep buffer count
				    client_width,
				    client_height,
				    DXGI_FORMAT_UNKNOWN,  // Keep format
				    0);

				if (SUCCEEDED(hr)) {
					// Recreate back buffer RTV
					wil::com_ptr<ID3D11Texture2D> back_buffer;
					c->render.swap_chain->GetBuffer(0, IID_PPV_ARGS(back_buffer.put()));
					sys->device->CreateRenderTargetView(back_buffer.get(), nullptr, c->render.back_buffer_rtv.put());

					U_LOG_W("Swap chain resized successfully to %ux%u", client_width, client_height);

					// Scale stereo texture proportionally to window/display ratio.
					// Skip during drag to avoid expensive texture reallocation every pixel.
					// The display processor handles mismatched stereo/target sizes via stretching.
					if (c->render.display_processor != nullptr && !in_size_move) {
						uint32_t disp_px_w = 0, disp_px_h = 0;
						int32_t disp_left = 0, disp_top = 0;

						if (xrt_display_processor_d3d11_get_display_pixel_info(
						        c->render.display_processor, &disp_px_w, &disp_px_h,
						        &disp_left, &disp_top) &&
						    disp_px_w > 0 && disp_px_h > 0) {

							// Compute base view dims from display pixel info using tile layout
							uint32_t base_vw = disp_px_w / sys->tile_columns;
							uint32_t base_vh = disp_px_h / sys->tile_rows;

							// Scale view dims by window/display ratio
							// This preserves aspect ratio during resize
							float ratio = fminf(
							    (float)client_width / (float)disp_px_w,
							    (float)client_height / (float)disp_px_h);
							if (ratio > 1.0f) {
								ratio = 1.0f;  // Don't upscale beyond recommended
							}

							uint32_t new_view_w = (uint32_t)((float)base_vw * ratio);
							uint32_t new_view_h = (uint32_t)((float)base_vh * ratio);
							uint32_t new_atlas_w = sys->tile_columns * new_view_w;
							uint32_t new_atlas_h = sys->tile_rows * new_view_h;

							// Only resize if significantly different (avoid churn)
							D3D11_TEXTURE2D_DESC current_desc = {};
							if (c->render.atlas_texture) {
								c->render.atlas_texture->GetDesc(&current_desc);
							}

							if (current_desc.Width != new_atlas_w || current_desc.Height != new_atlas_h) {
								U_LOG_W("Resizing atlas texture: %ux%u -> %ux%u (ratio=%.3f)",
								        current_desc.Width, current_desc.Height,
								        new_atlas_w, new_atlas_h, ratio);

								// Release old stereo texture resources
								c->render.atlas_rtv.reset();
								c->render.atlas_srv.reset();
								c->render.atlas_texture.reset();

								// Create new atlas texture
								D3D11_TEXTURE2D_DESC atlas_desc = {};
								atlas_desc.Width = new_atlas_w;
								atlas_desc.Height = new_atlas_h;
								atlas_desc.MipLevels = 1;
								atlas_desc.ArraySize = 1;
								atlas_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
								atlas_desc.SampleDesc.Count = 1;
								atlas_desc.Usage = D3D11_USAGE_DEFAULT;
								atlas_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

								HRESULT atlas_hr = sys->device->CreateTexture2D(
								    &atlas_desc, nullptr, c->render.atlas_texture.put());
								if (SUCCEEDED(atlas_hr)) {
									sys->device->CreateShaderResourceView(
									    c->render.atlas_texture.get(), nullptr, c->render.atlas_srv.put());
									sys->device->CreateRenderTargetView(
									    c->render.atlas_texture.get(), nullptr, c->render.atlas_rtv.put());

									// Update system view dimensions for rendering
									sys->view_width = new_view_w;
									sys->view_height = new_view_h;
									sys->display_width = new_atlas_w;
									sys->display_height = new_atlas_h;

									U_LOG_W("Atlas texture resized: view=%ux%u, atlas=%ux%u",
									        new_view_w, new_view_h, new_atlas_w, new_atlas_h);
								} else {
									U_LOG_E("Failed to resize atlas texture: 0x%08lx", atlas_hr);
								}
							}
						}
					}
				} else {
					U_LOG_E("Failed to resize swap chain: 0x%08lx", hr);
				}
			}
		}
	}

	// Clear stereo render target
	if (c->render.atlas_rtv) {
		float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		sys->context->ClearRenderTargetView(c->render.atlas_rtv.get(), clear_color);
	}

	// Sync hardware_display_3d and tile layout from device's active rendering mode
	sync_tile_layout(sys);
	if (sys->xdev != NULL && sys->xdev->hmd != NULL) {
		uint32_t idx = sys->xdev->hmd->active_rendering_mode_index;
		if (idx < sys->xdev->rendering_mode_count) {
			sys->hardware_display_3d = sys->xdev->rendering_modes[idx].hardware_display_3d;
		}
	}

	// Runtime-side 2D/3D toggle (V key) — polls qwerty driver each frame
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (sys->xsysd != NULL) {
		bool force_2d = false;
		bool toggled = qwerty_check_display_mode_toggle(
		    sys->xsysd->xdevs, sys->xsysd->xdev_count, &force_2d);
		if (toggled) {
			struct xrt_device *head = sys->xsysd->static_roles.head;
			if (head != nullptr && head->hmd != NULL) {
				if (force_2d) {
					// Save current 3D mode index before switching to 2D
					uint32_t cur = head->hmd->active_rendering_mode_index;
					if (cur < head->rendering_mode_count &&
					    head->rendering_modes[cur].hardware_display_3d) {
						sys->last_3d_mode_index = cur;
					}
					head->hmd->active_rendering_mode_index = 0;
				} else {
					// Restore last 3D mode index
					head->hmd->active_rendering_mode_index = sys->last_3d_mode_index;
				}
			}
			if (c->render.display_processor != nullptr) {
				xrt_display_processor_d3d11_request_display_mode(
				    c->render.display_processor, !force_2d);
			}
		}

		// Rendering mode change from qwerty 1/2/3 keys (disabled for legacy apps).
		if (!sys->base.info.legacy_app_tile_scaling) {
			int render_mode = -1;
			if (qwerty_check_rendering_mode_change(sys->xsysd->xdevs, sys->xsysd->xdev_count, &render_mode)) {
				struct xrt_device *head = sys->xsysd->static_roles.head;
				if (head != NULL) {
					xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, render_mode);
				}
			}
		}
	}
#endif

	// Get predicted eye positions (used for UI layers and HUD)
	struct xrt_eye_positions eye_pos = {};
	bool weaving_done = false;

	if (c->render.display_processor != nullptr) {
		xrt_display_processor_d3d11_get_predicted_eye_positions(
		    c->render.display_processor, &eye_pos);
	}
	if (!eye_pos.valid) {
		eye_pos.count = 2;
		eye_pos.eyes[0] = {-0.032f, 0.0f, 0.6f};
		eye_pos.eyes[1] = { 0.032f, 0.0f, 0.6f};
	}

	// Extract stereo pair for renderer (display processor still needs L/R)
	struct xrt_vec3 left_eye = {eye_pos.eyes[0].x, eye_pos.eyes[0].y, eye_pos.eyes[0].z};
	struct xrt_vec3 right_eye = {eye_pos.eyes[1].x, eye_pos.eyes[1].y, eye_pos.eyes[1].z};

	// Pre-compute whether there are UI overlay layers (quad/cylinder/equirect/cube).
	// Needed to decide if same-swapchain SBS can bypass the stereo texture entirely.
	bool has_ui_layers = false;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		switch (c->layer_accum.layers[i].data.type) {
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

	// Track zero-copy optimization: when all views are rendered into the same
	// swapchain texture with matching tiling layout, skip the blit and pass the
	// app's texture directly to the display processor.
	bool use_zero_copy = false;
	wil::com_ptr<ID3D11ShaderResourceView> zc_srv;
	ID3D11Texture2D *zc_tex = nullptr;
	uint32_t zc_view_w = 0, zc_view_h = 0;

	// Actual content dimensions from submitted rects - defaults to sys values,
	// overwritten per projection layer (may differ for legacy compromise-scale apps)
	uint32_t content_view_w = sys->view_width;
	uint32_t content_view_h = sys->view_height;

	// Render projection layers to stereo texture (via copy)
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];

		if (layer->data.type != XRT_LAYER_PROJECTION &&
		    layer->data.type != XRT_LAYER_PROJECTION_DEPTH) {
			continue;
		}

		// Determine view count: mono apps have 1 view, 3D mode uses all views
		uint32_t proj_view_count = layer->data.view_count;
		if (!sys->hardware_display_3d)
			proj_view_count = 1;
		if (proj_view_count > XRT_MAX_VIEWS)
			proj_view_count = XRT_MAX_VIEWS;

		// Extract per-view swapchains, textures, and image indices
		struct d3d11_service_swapchain *view_scs[XRT_MAX_VIEWS] = {};
		ID3D11Texture2D *view_textures[XRT_MAX_VIEWS] = {};
		uint32_t view_img_indices[XRT_MAX_VIEWS] = {};
		bool view_mutex_acquired[XRT_MAX_VIEWS] = {};
		D3D11_TEXTURE2D_DESC view_descs[XRT_MAX_VIEWS] = {};
		bool view_is_srgb[XRT_MAX_VIEWS] = {};

		bool views_valid = true;
		for (uint32_t eye = 0; eye < proj_view_count; eye++) {
			struct xrt_swapchain *xsc = layer->sc_array[eye];
			if (xsc == nullptr) {
				U_LOG_W("Projection layer %u missing swapchain for view %u", i, eye);
				views_valid = false;
				break;
			}
			view_scs[eye] = d3d11_service_swapchain_from_xrt(xsc);
			view_img_indices[eye] = layer->data.proj.v[eye].sub.image_index;

			if (view_img_indices[eye] >= view_scs[eye]->image_count) {
				U_LOG_W("Invalid image index in projection layer %u view %u", i, eye);
				views_valid = false;
				break;
			}
			view_textures[eye] = view_scs[eye]->images[view_img_indices[eye]].texture.get();
			if (view_textures[eye] == nullptr) {
				U_LOG_W("Missing texture in projection layer %u view %u", i, eye);
				views_valid = false;
				break;
			}
			view_textures[eye]->GetDesc(&view_descs[eye]);
			view_is_srgb[eye] = is_srgb_format(view_descs[eye].Format);
		}
		if (!views_valid) continue;

		// For service-created swapchains (WebXR), acquire KeyedMutex before reading
		const DWORD mutex_timeout_ms = 100;
		bool any_mutex_acquired = false;
		for (uint32_t eye = 0; eye < proj_view_count; eye++) {
			// Skip mutex for views sharing the same swapchain+image as a prior view
			bool already_locked = false;
			for (uint32_t prev = 0; prev < eye; prev++) {
				if (view_scs[eye] == view_scs[prev] && view_img_indices[eye] == view_img_indices[prev]) {
					already_locked = true;
					break;
				}
			}
			if (already_locked) continue;

			if (view_scs[eye]->service_created && view_scs[eye]->images[view_img_indices[eye]].keyed_mutex) {
				HRESULT hr = view_scs[eye]->images[view_img_indices[eye]].keyed_mutex->AcquireSync(0, mutex_timeout_ms);
				if (SUCCEEDED(hr)) {
					view_mutex_acquired[eye] = true;
					any_mutex_acquired = true;
				} else if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) {
					U_LOG_W("layer_commit: View %u mutex timeout (client still holding?)", eye);
				} else {
					U_LOG_W("layer_commit: Failed to acquire view %u mutex: 0x%08lx", eye, hr);
				}
			}
		}

		// Log projection layer info (first frame and every 60 frames)
		static uint32_t proj_log_count = 0;
		if (proj_log_count == 0 || proj_log_count % 60 == 0) {
			for (uint32_t eye = 0; eye < proj_view_count; eye++) {
				U_LOG_W("Projection layer %u view %u/%u: rect=(%d,%d %dx%d) array=%u fmt=%u(srgb=%d)",
				        i, eye, proj_view_count,
				        layer->data.proj.v[eye].sub.rect.offset.w, layer->data.proj.v[eye].sub.rect.offset.h,
				        layer->data.proj.v[eye].sub.rect.extent.w, layer->data.proj.v[eye].sub.rect.extent.h,
				        layer->data.proj.v[eye].sub.array_index,
				        view_descs[eye].Format, view_is_srgb[eye]);
			}
			U_LOG_W("  atlas_texture=%ux%u, view_width=%u, view_height=%u",
			        sys->display_width, sys->display_height, sys->view_width, sys->view_height);
		}
		proj_log_count++;

		// Zero-copy optimization: all views reference the same swapchain texture,
		// sub-rects match tiling layout, and texture matches content dimensions.
		// Skip the blit and pass the app's texture directly to the display processor.
		bool zero_copy = false;

		if (proj_view_count > 1 && !has_ui_layers && !any_mutex_acquired) {
			// Check all views reference the same swapchain image
			bool all_same = true;
			for (uint32_t eye = 1; eye < proj_view_count; eye++) {
				if (view_scs[eye] != view_scs[0] || view_img_indices[eye] != view_img_indices[0]) {
					all_same = false;
					break;
				}
			}

			if (all_same) {
				// Use u_tiling_can_zero_copy to verify sub-rects match tiling layout
				int32_t rect_xs[XRT_MAX_VIEWS], rect_ys[XRT_MAX_VIEWS];
				uint32_t rect_ws[XRT_MAX_VIEWS], rect_hs[XRT_MAX_VIEWS];
				for (uint32_t eye = 0; eye < proj_view_count; eye++) {
					rect_xs[eye] = layer->data.proj.v[eye].sub.rect.offset.w;
					rect_ys[eye] = layer->data.proj.v[eye].sub.rect.offset.h;
					rect_ws[eye] = layer->data.proj.v[eye].sub.rect.extent.w;
					rect_hs[eye] = layer->data.proj.v[eye].sub.rect.extent.h;
				}

				// Get active rendering mode for zero-copy check
				struct xrt_device *xdev_head = (sys->xsysd != nullptr) ? sys->xsysd->static_roles.head : nullptr;
				const struct xrt_rendering_mode *active_mode = nullptr;
				if (xdev_head != nullptr && xdev_head->hmd != nullptr) {
					uint32_t idx = xdev_head->hmd->active_rendering_mode_index;
					if (idx < xdev_head->rendering_mode_count) {
						active_mode = &xdev_head->rendering_modes[idx];
					}
				}

				if (active_mode != nullptr &&
				    u_tiling_can_zero_copy(proj_view_count, rect_xs, rect_ys, rect_ws, rect_hs,
				                           view_descs[0].Width, view_descs[0].Height, active_mode)) {
					// Texture matches atlas dims exactly — zero-copy is safe
					D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
					srv_desc.Format = view_is_srgb[0] ? get_srgb_format(view_descs[0].Format) : view_descs[0].Format;
					srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
					srv_desc.Texture2D.MipLevels = 1;
					srv_desc.Texture2D.MostDetailedMip = 0;

					wil::com_ptr<ID3D11ShaderResourceView> app_srv;
					HRESULT hr = sys->device->CreateShaderResourceView(view_textures[0], &srv_desc, app_srv.put());
					if (SUCCEEDED(hr)) {
						zero_copy = true;
						use_zero_copy = true;
						zc_srv = std::move(app_srv);
						zc_tex = view_textures[0];
						zc_view_w = static_cast<uint32_t>(rect_ws[0]);
						zc_view_h = static_cast<uint32_t>(rect_hs[0]);

						static bool logged_zc = false;
						if (!logged_zc) {
							U_LOG_W("Zero-copy atlas: skipping blit, view=%ux%u, views=%u, fmt=0x%X",
							        rect_ws[0], rect_hs[0], proj_view_count, srv_desc.Format);
							logged_zc = true;
						}
					}
				}
			}
		}

		if (!zero_copy) {
		// Blit each view into its atlas tile position
		static bool logged_blit_path = false;
		if (!logged_blit_path) {
			U_LOG_W("Blit path: srgb=%d, blit_vs=%p -> %s",
			        view_is_srgb[0], (void*)sys->blit_vs.get(),
			        (view_is_srgb[0] && sys->blit_vs) ? "SHADER BLIT (linear output)" : "COPY (no conversion)");
			logged_blit_path = true;
		}

		for (uint32_t eye = 0; eye < proj_view_count; eye++) {
			float src_x = static_cast<float>(layer->data.proj.v[eye].sub.rect.offset.w);
			float src_y = static_cast<float>(layer->data.proj.v[eye].sub.rect.offset.h);
			float src_w = static_cast<float>(layer->data.proj.v[eye].sub.rect.extent.w);
			float src_h = static_cast<float>(layer->data.proj.v[eye].sub.rect.extent.h);

			uint32_t tile_x, tile_y;
			u_tiling_view_origin(eye, sys->tile_columns,
			                     sys->view_width, sys->view_height,
			                     &tile_x, &tile_y);

			if (view_is_srgb[eye] && sys->blit_vs && view_scs[eye]->images[view_img_indices[eye]].srv) {
				wil::com_ptr<ID3D11ShaderResourceView> srgb_srv;
				D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
				srv_desc.Format = get_srgb_format(view_descs[eye].Format);
				srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				srv_desc.Texture2D.MipLevels = 1;
				srv_desc.Texture2D.MostDetailedMip = 0;

				HRESULT hr = sys->device->CreateShaderResourceView(view_textures[eye], &srv_desc, srgb_srv.put());
				if (SUCCEEDED(hr)) {
					blit_to_atlas_texture(sys, &c->render, srgb_srv.get(),
					                       src_x, src_y, src_w, src_h,
					                       static_cast<float>(view_descs[eye].Width), static_cast<float>(view_descs[eye].Height),
					                       static_cast<float>(tile_x), static_cast<float>(tile_y),
					                       true);  // is_srgb = true
				} else {
					U_LOG_W("Failed to create SRGB SRV for view %u, falling back to copy", eye);
					D3D11_BOX box = {};
					box.left = static_cast<UINT>(src_x);
					box.top = static_cast<UINT>(src_y);
					box.right = static_cast<UINT>(src_x + src_w);
					box.bottom = static_cast<UINT>(src_y + src_h);
					box.front = 0;
					box.back = 1;
					sys->context->CopySubresourceRegion(c->render.atlas_texture.get(), 0,
					                                     tile_x, tile_y, 0,
					                                     view_textures[eye], layer->data.proj.v[eye].sub.array_index, &box);
				}
			} else {
				// Non-SRGB: use fast CopySubresourceRegion
				D3D11_BOX box = {};
				box.left = static_cast<UINT>(src_x);
				box.top = static_cast<UINT>(src_y);
				box.right = static_cast<UINT>(src_x + src_w);
				box.bottom = static_cast<UINT>(src_y + src_h);
				box.front = 0;
				box.back = 1;

				sys->context->CopySubresourceRegion(
				    c->render.atlas_texture.get(),
				    0,                            // dst subresource
				    tile_x, tile_y, 0,            // dst x, y, z (tile position)
				    view_textures[eye],
				    layer->data.proj.v[eye].sub.array_index,  // src subresource
				    &box);
			}
		}
		} // !zero_copy

		// Track actual content dimensions from submitted rects (may differ from
		// sys->view_width/height for legacy apps that render at compromise scale)
		content_view_w = static_cast<uint32_t>(layer->data.proj.v[0].sub.rect.extent.w);
		content_view_h = static_cast<uint32_t>(layer->data.proj.v[0].sub.rect.extent.h);

		// Release KeyedMutex after reading
		for (uint32_t eye = 0; eye < proj_view_count; eye++) {
			if (view_mutex_acquired[eye]) {
				view_scs[eye]->images[view_img_indices[eye]].keyed_mutex->ReleaseSync(0);
			}
		}

		U_LOG_T("Rendered projection layer %u", i);
	}

	// Render UI layers if any exist and shaders are ready
	if (has_ui_layers && sys->quad_vs) {
		// Bind per-client stereo render target
		ID3D11RenderTargetView *rtvs[] = {c->render.atlas_rtv.get()};
		sys->context->OMSetRenderTargets(1, rtvs, nullptr);

		// Set common rendering state
		sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		sys->context->IASetInputLayout(nullptr);  // Using SV_VertexID
		sys->context->RSSetState(sys->rasterizer_state.get());
		sys->context->OMSetDepthStencilState(sys->depth_disabled.get(), 0);

		// Create default view poses and FOVs for each view
		uint32_t ui_view_count = sys->hardware_display_3d
		    ? (sys->tile_columns * sys->tile_rows) : 1;
		if (ui_view_count > XRT_MAX_VIEWS)
			ui_view_count = XRT_MAX_VIEWS;

		struct xrt_pose view_poses[XRT_MAX_VIEWS];
		struct xrt_fov fovs[XRT_MAX_VIEWS];

		// Use eye positions from display processor (interpolate for N views)
		const float fov_angle = 0.785f;  // ~45 degrees
		for (uint32_t view = 0; view < ui_view_count; view++) {
			view_poses[view].orientation.x = 0.0f;
			view_poses[view].orientation.y = 0.0f;
			view_poses[view].orientation.z = 0.0f;
			view_poses[view].orientation.w = 1.0f;

			// Use eye position if available, fall back to interpolated stereo baseline
			if (view < eye_pos.count) {
				view_poses[view].position.x = eye_pos.eyes[view].x;
				view_poses[view].position.y = eye_pos.eyes[view].y;
				view_poses[view].position.z = eye_pos.eyes[view].z;
			} else if (view == 0) {
				view_poses[view].position = left_eye;
			} else {
				view_poses[view].position = right_eye;
			}

			fovs[view].angle_left = -fov_angle;
			fovs[view].angle_right = fov_angle;
			fovs[view].angle_up = fov_angle;
			fovs[view].angle_down = -fov_angle;
		}
		for (uint32_t view_index = 0; view_index < ui_view_count; view_index++) {
			// Set viewport for this view
			D3D11_VIEWPORT viewport = {};
			if (!sys->hardware_display_3d) {
				// MONO: use output (window) dimensions so 2D content
				// fills the full window, capped to stereo texture size.
				uint32_t mono_w = (sys->output_width < sys->display_width)
				                      ? sys->output_width : sys->display_width;
				uint32_t mono_h = (sys->output_height < sys->display_height)
				                      ? sys->output_height : sys->display_height;
				viewport.TopLeftX = 0.0f;
				viewport.Width = static_cast<float>(mono_w);
				viewport.Height = static_cast<float>(mono_h);
			} else {
				// STEREO: tiled atlas layout
				uint32_t tile_x, tile_y;
				u_tiling_view_origin(view_index, sys->tile_columns,
				                     sys->view_width, sys->view_height,
				                     &tile_x, &tile_y);
				viewport.TopLeftX = static_cast<float>(tile_x);
				viewport.TopLeftY = static_cast<float>(tile_y);
				viewport.Width = static_cast<float>(sys->view_width);
				viewport.Height = static_cast<float>(sys->view_height);
			}
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

	// During drag, synchronize with the window thread's WM_PAINT cycle.
	// This ensures the window position is stable between weave() and Present(),
	// so the interlacing pattern matches the actual displayed position.
	if (c->render.owns_window && c->render.window != nullptr &&
	    comp_d3d11_window_is_in_size_move(c->render.window)) {
		comp_d3d11_window_wait_for_paint(c->render.window);
	}

	// Select display processor input: zero-copy from app's swapchain, or intermediate atlas_texture
	ID3D11ShaderResourceView *input_srv = use_zero_copy
	    ? zc_srv.get() : c->render.atlas_srv.get();
	uint32_t input_view_w = use_zero_copy ? zc_view_w : content_view_w;
	uint32_t input_view_h = use_zero_copy ? zc_view_h : content_view_h;

	// Process stereo texture through display processor for display output
	// Skip processing for mono — display is in 2D mode, no interlacing needed
	if (sys->hardware_display_3d && c->render.display_processor != nullptr && input_srv) {
		// Bind back buffer as output
		ID3D11RenderTargetView *rtvs[] = {c->render.back_buffer_rtv.get()};
		sys->context->OMSetRenderTargets(1, rtvs, nullptr);

		// Get actual back buffer dimensions for viewport
		uint32_t back_buffer_width = sys->output_width;
		uint32_t back_buffer_height = sys->output_height;
		if (c->render.back_buffer_rtv) {
			wil::com_ptr<ID3D11Resource> bb_resource;
			c->render.back_buffer_rtv->GetResource(bb_resource.put());
			wil::com_ptr<ID3D11Texture2D> bb_texture;
			if (SUCCEEDED(bb_resource->QueryInterface(IID_PPV_ARGS(bb_texture.put())))) {
				D3D11_TEXTURE2D_DESC bb_desc = {};
				bb_texture->GetDesc(&bb_desc);
				back_buffer_width = bb_desc.Width;
				back_buffer_height = bb_desc.Height;
			}
		}

		xrt_display_processor_d3d11_process_atlas(
		    c->render.display_processor, sys->context.get(), input_srv,
		    input_view_w, input_view_h, sys->tile_columns, sys->tile_rows,
		    DXGI_FORMAT_R8G8B8A8_UNORM, back_buffer_width, back_buffer_height);
		weaving_done = true;
	} else {
		// No display processor or mono — copy to back buffer directly
		if (c->render.back_buffer_rtv) {
			wil::com_ptr<ID3D11Resource> back_buffer;
			c->render.back_buffer_rtv->GetResource(back_buffer.put());

			if (!sys->hardware_display_3d) {
				// MONO / forced 2D: stretch-blit left eye to fill entire back buffer.
				// The source is SBS (2*view_width x view_height); we sample only the
				// left half and stretch it to the full back buffer via a GPU blit.
				ID3D11ShaderResourceView *src_srv = nullptr;
				uint32_t eye_w = 0, eye_h = 0;
				uint32_t tex_w = 0, tex_h = 0;

				if (use_zero_copy && zc_srv) {
					src_srv = zc_srv.get();
					eye_w = zc_view_w;
					eye_h = zc_view_h;
					tex_w = sys->tile_columns * zc_view_w;
					tex_h = sys->tile_rows * zc_view_h;
				} else if (c->render.atlas_srv) {
					src_srv = c->render.atlas_srv.get();
					eye_w = content_view_w;
					eye_h = content_view_h;
					tex_w = sys->tile_columns * content_view_w;
					tex_h = sys->tile_rows * content_view_h;
				}

				if (src_srv != nullptr) {
					// Bind back buffer as render target
					ID3D11RenderTargetView *rtvs[] = {c->render.back_buffer_rtv.get()};
					sys->context->OMSetRenderTargets(1, rtvs, nullptr);

					// Viewport fills the entire back buffer — stretches the quad
					D3D11_VIEWPORT vp = {};
					vp.Width = static_cast<float>(sys->output_width);
					vp.Height = static_cast<float>(sys->output_height);
					vp.MaxDepth = 1.0f;
					sys->context->RSSetViewports(1, &vp);

					// Pipeline state
					sys->context->IASetPrimitiveTopology(
					    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
					sys->context->IASetInputLayout(nullptr);
					sys->context->VSSetShader(sys->blit_vs.get(), nullptr, 0);
					sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0);
					sys->context->RSSetState(sys->rasterizer_state.get());
					sys->context->OMSetDepthStencilState(sys->depth_disabled.get(), 0);
					sys->context->OMSetBlendState(
					    sys->blend_opaque.get(), nullptr, 0xFFFFFFFF);
					sys->context->PSSetSamplers(
					    0, 1, sys->sampler_linear.addressof());
					sys->context->PSSetShaderResources(0, 1, &src_srv);

					// Blit constants: dst_size = eye dims so NDC maps fullscreen;
					// src_size = actual SBS dims so UVs sample only the left eye.
					BlitConstants bc = {};
					bc.src_rect[0] = 0.0f;
					bc.src_rect[1] = 0.0f;
					bc.src_rect[2] = static_cast<float>(eye_w);
					bc.src_rect[3] = static_cast<float>(eye_h);
					bc.dst_offset[0] = 0.0f;
					bc.dst_offset[1] = 0.0f;
					bc.src_size[0] = static_cast<float>(tex_w);
					bc.src_size[1] = static_cast<float>(tex_h);
					bc.dst_size[0] = static_cast<float>(eye_w);
					bc.dst_size[1] = static_cast<float>(eye_h);
					bc.convert_srgb = 0.0f;

					D3D11_MAPPED_SUBRESOURCE mapped;
					HRESULT hr = sys->context->Map(
					    sys->blit_constant_buffer.get(), 0,
					    D3D11_MAP_WRITE_DISCARD, 0, &mapped);
					if (SUCCEEDED(hr)) {
						memcpy(mapped.pData, &bc, sizeof(bc));
						sys->context->Unmap(
						    sys->blit_constant_buffer.get(), 0);
					}
					sys->context->VSSetConstantBuffers(
					    0, 1, sys->blit_constant_buffer.addressof());
					sys->context->PSSetConstantBuffers(
					    0, 1, sys->blit_constant_buffer.addressof());

					sys->context->Draw(4, 0);

					// Unbind SRV to prevent hazard
					ID3D11ShaderResourceView *null_srv = nullptr;
					sys->context->PSSetShaderResources(0, 1, &null_srv);
				}
			} else if (use_zero_copy && zc_tex) {
				// Zero-copy: copy app's atlas region to back buffer
				D3D11_BOX src_box = {0, 0, 0, sys->tile_columns * input_view_w, sys->tile_rows * input_view_h, 1};
				sys->context->CopySubresourceRegion(
				    back_buffer.get(), 0, 0, 0, 0,
				    zc_tex, 0, &src_box);
			} else if (c->render.atlas_texture) {
				sys->context->CopyResource(back_buffer.get(), c->render.atlas_texture.get());
			}
		}
	}

	// Render HUD overlay (post-weave, pre-present)
	d3d11_service_render_hud(sys, &c->render, weaving_done, &eye_pos);

	// Present to display
	if (c->render.swap_chain) {
		c->render.swap_chain->Present(1, 0);  // VSync
	}

	// Signal WM_PAINT that the frame is done (unblocks modal drag loop)
	if (c->render.owns_window && c->render.window != nullptr) {
		comp_d3d11_window_signal_paint_done(c->render.window);
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
	struct d3d11_service_system *sys = c->sys;

	U_LOG_W("Destroying D3D11 service compositor for client");

	// Clear active compositor if it's this one
	{
		std::lock_guard<std::mutex> lock(sys->active_compositor_mutex);
		if (sys->active_compositor == c) {
			sys->active_compositor = nullptr;
		}
	}

	// Clean up per-client render resources (window, swap chain, display processor)
	fini_client_render_resources(&c->render);

	delete c;
}


/*
 *
 * System compositor functions
 *
 */

static xrt_result_t
system_set_state(struct xrt_system_compositor *xsc, struct xrt_compositor *xc, bool visible, bool focused)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	// Only push event if state actually changed
	if (c->state_visible != visible || c->state_focused != focused) {
		c->state_visible = visible;
		c->state_focused = focused;

		union xrt_session_event xse = XRT_STRUCT_INIT;
		xse.type = XRT_SESSION_EVENT_STATE_CHANGE;
		xse.state.visible = visible;
		xse.state.focused = focused;

		U_LOG_W("D3D11 service: pushing state change event (visible=%d, focused=%d)", visible, focused);
		return xrt_session_event_sink_push(c->xses, &xse);
	}

	return XRT_SUCCESS;
}

static xrt_result_t
system_set_z_order(struct xrt_system_compositor *xsc, struct xrt_compositor *xc, int64_t z_order)
{
	// D3D11 service doesn't need z_order handling for single-client case
	(void)xsc;
	(void)xc;
	(void)z_order;
	return XRT_SUCCESS;
}

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

	// Store session event sink for pushing state change events
	c->xses = xses;
	c->state_visible = false;
	c->state_focused = false;
	c->window_closed = false;
	c->exit_request_sent = false;
	c->window_closed_frame_count = 0;

	// Initialize layer accumulator
	std::memset(&c->layer_accum, 0, sizeof(c->layer_accum));

	// Initialize per-client render resources (window, swap chain, display processor)
	// Get external window handle if app provided one via XR_EXT_win32_window_binding
	void *external_hwnd = nullptr;
	if (xsi != nullptr) {
		external_hwnd = xsi->external_window_handle;
	}

	xrt_result_t res_ret = init_client_render_resources(sys, external_hwnd, sys->xsysd, &c->render);
	if (res_ret != XRT_SUCCESS) {
		U_LOG_E("Failed to initialize client render resources");
		delete c;
		return res_ret;
	}

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

	// Set initial visibility/focus state (will be returned to client via IPC)
	// This avoids race condition where client must poll events before these are set
	c->base.base.info.initial_visible = true;
	c->base.base.info.initial_focused = true;

	U_LOG_W("D3D11 service: created native compositor for client");

	*out_xcn = &c->base;
	return XRT_SUCCESS;
}

static void
system_destroy(struct xrt_system_compositor *xsysc)
{
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);

	U_LOG_I("Destroying D3D11 service system compositor");

	// NOTE: Per-client display processors are cleaned up in fini_client_render_resources()
	// when each client disconnects. System has no display processor anymore.

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

	// Clean up blit shader resources
	sys->blit_constant_buffer.reset();
	sys->blit_ps.reset();
	sys->blit_vs.reset();

	// NOTE: Per-client resources (window, swap_chain, atlas_texture, display processor)
	// are cleaned up in fini_client_render_resources() when each client disconnects.
	// System only needs to clean up shared resources (device, shaders, etc.)

	sys->dxgi_factory.reset();
	sys->context.reset();
	sys->device.reset();

	delete sys;
}


/*
 *
 * Exported functions
 *
 */

extern "C" xrt_result_t
comp_d3d11_service_create_system(struct xrt_device *xdev,
                                 struct xrt_system_devices *xsysd,
                                 struct xrt_system_compositor **out_xsysc)
{
	U_LOG_W("Creating D3D11 service system compositor (xsysd=%p)", (void *)xsysd);

	// Allocate system compositor
	struct d3d11_service_system *sys = new d3d11_service_system();
	std::memset(&sys->base, 0, sizeof(sys->base));

	sys->xdev = xdev;
	sys->log_level = U_LOGGING_INFO;
	sys->hardware_display_3d = true;
	sys->last_3d_mode_index = 1;

	// Default tile layout (stereo side-by-side) and display dimensions
	sys->tile_columns = 2;
	sys->tile_rows = 1;
	sync_tile_layout(sys);
	sys->output_width = 1920;
	sys->output_height = 1080;
	sys->view_width = sys->output_width / sys->tile_columns;
	sys->view_height = sys->output_height / sys->tile_rows;
	sys->display_width = sys->tile_columns * sys->view_width;
	sys->display_height = sys->tile_rows * sys->view_height;
	sys->refresh_rate = 60.0f;
	// NOTE: Display processor queries happen after D3D11 device creation (below).

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

	// Get adapter LUID and set it in system info so clients use the same GPU
	DXGI_ADAPTER_DESC adapter_desc = {};
	hr = adapter->GetDesc(&adapter_desc);
	if (SUCCEEDED(hr)) {
		// Copy LUID to system info (LUID is 8 bytes: LowPart + HighPart)
		static_assert(sizeof(adapter_desc.AdapterLuid) == XRT_LUID_SIZE, "LUID size mismatch");
		std::memcpy(sys->base.info.client_d3d_deviceLUID.data, &adapter_desc.AdapterLuid, XRT_LUID_SIZE);
		sys->base.info.client_d3d_deviceLUID_valid = true;
		U_LOG_W("D3D11 service compositor using adapter LUID: %08lx-%08lx",
		        adapter_desc.AdapterLuid.HighPart, adapter_desc.AdapterLuid.LowPart);
	} else {
		U_LOG_W("Failed to get adapter LUID, D3D clients may use wrong GPU");
		sys->base.info.client_d3d_deviceLUID_valid = false;
	}

	hr = adapter->GetParent(IID_PPV_ARGS(sys->dxgi_factory.put()));
	if (FAILED(hr)) {
		U_LOG_E("Failed to get DXGI factory: 0x%08lx", hr);
		delete sys;
		return XRT_ERROR_VULKAN;
	}

	// Store system devices for passing to per-client windows
	sys->xsysd = xsysd;

	// Query display dimensions from display processor (if factory is available).
	// Create a temporary display processor with NULL window to query pixel info,
	// then destroy it. Per-client display processors are created later with real windows.
	if (sys->base.info.dp_factory_d3d11 != NULL) {
		auto factory = (xrt_dp_factory_d3d11_fn_t)sys->base.info.dp_factory_d3d11;
		struct xrt_display_processor_d3d11 *tmp_dp = nullptr;
		xrt_result_t dp_ret = factory(sys->device.get(), sys->context.get(), NULL, &tmp_dp);
		if (dp_ret == XRT_SUCCESS && tmp_dp != nullptr) {
			uint32_t disp_px_w = 0, disp_px_h = 0;
			int32_t disp_left = 0, disp_top = 0;
			if (xrt_display_processor_d3d11_get_display_pixel_info(
			        tmp_dp, &disp_px_w, &disp_px_h, &disp_left, &disp_top) &&
			    disp_px_w > 0 && disp_px_h > 0) {
				// Compute per-view dims using tile layout from active rendering mode
				sys->view_width = disp_px_w / sys->tile_columns;
				sys->view_height = disp_px_h / sys->tile_rows;
				sys->display_width = sys->tile_columns * sys->view_width;
				sys->display_height = sys->tile_rows * sys->view_height;
				sys->output_width = disp_px_w;
				sys->output_height = disp_px_h;
				U_LOG_W("Display processor pixel info: %ux%u, view=%ux%u per eye (tiles %ux%u)",
				        disp_px_w, disp_px_h, sys->view_width, sys->view_height,
				        sys->tile_columns, sys->tile_rows);
			} else {
				U_LOG_W("Display processor created but pixel info unavailable, using defaults");
			}

			// Query display physical dimensions
			float w_m = 0.0f, h_m = 0.0f;
			if (xrt_display_processor_d3d11_get_display_dimensions(tmp_dp, &w_m, &h_m)) {
				sys->base.info.display_width_m = w_m;
				sys->base.info.display_height_m = h_m;
			}

			xrt_display_processor_d3d11_destroy(&tmp_dp);
		} else {
			U_LOG_W("Temporary display processor creation failed, using default dimensions");
		}
	}

	// Create layer shaders and resources for UI layer rendering
	// These are shared across all clients
	if (!create_layer_shaders(sys)) {
		U_LOG_W("Failed to create layer shaders, UI layers will not render");
		// Don't fail - projection layers will still work
	} else if (!create_layer_resources(sys)) {
		U_LOG_W("Failed to create layer resources, UI layers will not render");
		// Don't fail - projection layers will still work
	}

	// NOTE: Window, swap chain, and display processor are now created per-client
	// in system_create_native_compositor() -> init_client_render_resources()
	// This allows the IPC service to start without a window until a client connects.

	// Set up system compositor vtable
	sys->base.create_native_compositor = system_create_native_compositor;
	sys->base.destroy = system_destroy;

	// Set up multi-compositor control for session state management
	sys->xmcc.set_state = system_set_state;
	sys->xmcc.set_z_order = system_set_z_order;
	sys->xmcc.set_main_app_visibility = NULL;  // Not needed for single client
	sys->xmcc.notify_loss_pending = NULL;
	sys->xmcc.notify_lost = NULL;
	sys->xmcc.notify_display_refresh_changed = NULL;
	sys->base.xmcc = &sys->xmcc;

	// Fill system compositor info
	sys->base.info.max_layers = XRT_MAX_LAYERS;
	sys->base.info.views[0].recommended.width_pixels = sys->view_width;
	sys->base.info.views[0].recommended.height_pixels = sys->view_height;
	sys->base.info.views[0].max.width_pixels = sys->view_width * 2;
	sys->base.info.views[0].max.height_pixels = sys->view_height * 2;
	sys->base.info.views[1] = sys->base.info.views[0];

	// Set supported blend modes (Chrome WebXR requires at least OPAQUE)
	sys->base.info.supported_blend_modes[0] = XRT_BLEND_MODE_OPAQUE;
	sys->base.info.supported_blend_mode_count = 1;

	// Populate display info for XR_EXT_display_info
	// (display_width_m and display_height_m are already set above from the temporary display processor query)
	if (sys->output_width > 0 && sys->output_height > 0) {
		sys->base.info.recommended_view_scale_x = (float)sys->view_width / (float)sys->output_width;
		sys->base.info.recommended_view_scale_y = (float)sys->view_height / (float)sys->output_height;
		sys->base.info.display_pixel_width = sys->output_width;
		sys->base.info.display_pixel_height = sys->output_height;
	}

	U_LOG_W("D3D11 service system compositor created: view=%ux%u/eye, stereo=%ux%u, output=%ux%u @ %.0fHz",
	        sys->view_width, sys->view_height,
	        sys->display_width, sys->display_height,
	        sys->output_width, sys->output_height, sys->refresh_rate);

	*out_xsysc = &sys->base;
	return XRT_SUCCESS;
}

/*
 *
 * Helper functions for IPC server to get display processor data
 *
 */

bool
comp_d3d11_service_is_d3d11_service(struct xrt_system_compositor *xsysc)
{
	if (xsysc == NULL) {
		return false;
	}
	// Check by comparing function pointers - this identifies our compositor type
	bool is_d3d11_service = (xsysc->create_native_compositor == system_create_native_compositor);

	// Log first call for debugging
	static bool first_call = true;
	if (first_call) {
		first_call = false;
		U_LOG_W("comp_d3d11_service_is_d3d11_service: xsysc=%p, create_native_compositor=%p, expected=%p, match=%s",
		        (void*)xsysc,
		        (void*)xsysc->create_native_compositor,
		        (void*)system_create_native_compositor,
		        is_d3d11_service ? "YES" : "NO");
	}
	return is_d3d11_service;
}

bool
comp_d3d11_service_get_predicted_eye_positions(struct xrt_system_compositor *xsysc,
                                                struct xrt_vec3 *out_left,
                                                struct xrt_vec3 *out_right)
{
	if (xsysc == NULL || out_left == NULL || out_right == NULL) {
		return false;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);

	// Get the active compositor's display processor for eye position prediction
	struct xrt_display_processor_d3d11 *dp = nullptr;
	{
		std::lock_guard<std::mutex> lock(sys->active_compositor_mutex);
		if (sys->active_compositor != nullptr &&
		    sys->active_compositor->render.display_processor != nullptr) {
			dp = sys->active_compositor->render.display_processor;
		}
	}

	if (dp != nullptr) {
		struct xrt_eye_positions eyes;
		if (xrt_display_processor_d3d11_get_predicted_eye_positions(dp, &eyes) && eyes.valid) {
			out_left->x = eyes.eyes[0].x;
			out_left->y = eyes.eyes[0].y;
			out_left->z = eyes.eyes[0].z;
			out_right->x = eyes.eyes[1].x;
			out_right->y = eyes.eyes[1].y;
			out_right->z = eyes.eyes[1].z;

			// Log periodically for debugging
			static int log_counter = 0;
			if (++log_counter >= 60) {
				log_counter = 0;
				U_LOG_W("IPC eye positions (from display processor): L=(%.3f,%.3f,%.3f) R=(%.3f,%.3f,%.3f)",
				        out_left->x, out_left->y, out_left->z,
				        out_right->x, out_right->y, out_right->z);
			}
			return true;
		}
	}

	// Log if we have no active display processor
	static bool logged_no_dp = false;
	if (!logged_no_dp) {
		logged_no_dp = true;
		U_LOG_W("comp_d3d11_service_get_predicted_eye_positions: no active display processor available");
	}

	return false;
}

bool
comp_d3d11_service_get_display_dimensions(struct xrt_system_compositor *xsysc,
                                           float *out_width_m,
                                           float *out_height_m)
{
	if (xsysc == NULL || out_width_m == NULL || out_height_m == NULL) {
		return false;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);

	// Try to get display dimensions from active compositor's display processor
	struct xrt_display_processor_d3d11 *dp = nullptr;
	{
		std::lock_guard<std::mutex> lock(sys->active_compositor_mutex);
		if (sys->active_compositor != nullptr &&
		    sys->active_compositor->render.display_processor != nullptr) {
			dp = sys->active_compositor->render.display_processor;
		}
	}

	if (dp != nullptr) {
		if (xrt_display_processor_d3d11_get_display_dimensions(dp, out_width_m, out_height_m)) {
			return true;
		}
	}

	// Fall back to values cached in system compositor info (set during init)
	if (sys->base.info.display_width_m > 0.0f && sys->base.info.display_height_m > 0.0f) {
		*out_width_m = sys->base.info.display_width_m;
		*out_height_m = sys->base.info.display_height_m;
		return true;
	}

	return false;
}

bool
comp_d3d11_service_owns_window(struct xrt_system_compositor *xsysc)
{
	// NOTE: With per-client windows, this function now applies to the default
	// behavior. IPC clients (no external_window_handle) always get Monado windows.
	// Native compositor clients can provide their own via XR_EXT_win32_window_binding.
	// For IPC path (which calls this), we always create windows, so return true.
	(void)xsysc;
	return true;
}

bool
comp_d3d11_service_window_is_valid(struct xrt_system_compositor *xsysc)
{
	// NOTE: With per-client windows, window validity is now per-client.
	// The IPC server no longer needs a single window validity check.
	// Each client's window lifecycle is handled when the client disconnects.
	// Always return true - the service doesn't maintain a global window anymore.
	(void)xsysc;
	return true;
}
