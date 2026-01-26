// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 service compositor implementation.
 * @author David Fattal
 * @ingroup comp_d3d11_service
 */

#include "comp_d3d11_service.h"

#include "xrt/xrt_handles.h"
#include "xrt/xrt_config_have.h"
#include "xrt/xrt_limits.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "os/os_time.h"

#include "util/comp_layer_accum.h"
#include "multi/comp_multi_interface.h"

#ifdef XRT_HAVE_LEIA_SR
#include "leiasr/leiasr_d3d11.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <wil/com.h>
#include <wil/result.h>

#include <cstdlib>
#include <cstring>
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
 */
struct d3d11_service_swapchain
{
	//! Base swapchain - must be first!
	struct xrt_swapchain base;

	//! Parent compositor
	struct d3d11_service_compositor *comp;

	//! Swapchain images
	struct d3d11_service_image images[XRT_MAX_SWAPCHAIN_IMAGES];

	//! Image count
	uint32_t image_count;

	//! Creation info
	struct xrt_swapchain_create_info info;
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

	//! Self-created window for display output
	HWND hwnd;

	//! Multi-compositor control for managing multiple clients
	struct comp_multi_system_compositor *msc;

#ifdef XRT_HAVE_LEIA_SR
	//! SR weaver for light field display
	struct leiasr_d3d11 *weaver;
#endif

	//! Display dimensions
	uint32_t display_width;
	uint32_t display_height;

	//! View dimensions (stereo)
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
d3d11_service_swapchain(struct xrt_swapchain *xsc)
{
	return reinterpret_cast<struct d3d11_service_swapchain *>(xsc);
}

static inline struct d3d11_service_compositor *
d3d11_service_compositor(struct xrt_compositor *xc)
{
	return reinterpret_cast<struct d3d11_service_compositor *>(xc);
}

static inline struct d3d11_service_system *
d3d11_service_system(struct xrt_system_compositor *xsysc)
{
	return reinterpret_cast<struct d3d11_service_system *>(xsysc);
}


/*
 *
 * Swapchain functions
 *
 */

static void
swapchain_destroy(struct xrt_swapchain *xsc)
{
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain(xsc);

	// Release all images
	for (uint32_t i = 0; i < sc->image_count; i++) {
		if (sc->images[i].mutex_acquired && sc->images[i].keyed_mutex) {
			sc->images[i].keyed_mutex->ReleaseSync(0);
		}
		sc->images[i].srv.reset();
		sc->images[i].keyed_mutex.reset();
		sc->images[i].texture.reset();
	}

	delete sc;
}

static xrt_result_t
swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain(xsc);

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
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain(xsc);

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
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain(xsc);

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
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain(xsc);

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

static xrt_result_t
compositor_create_swapchain(struct xrt_compositor *xc,
                             const struct xrt_swapchain_create_info *info,
                             struct xrt_swapchain **out_xsc)
{
	// Service compositor doesn't create swapchains directly
	// Clients create and share them via import_swapchain
	return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
}

static xrt_result_t
compositor_import_swapchain(struct xrt_compositor *xc,
                             const struct xrt_swapchain_create_info *info,
                             struct xrt_image_native *native_images,
                             uint32_t image_count,
                             struct xrt_swapchain **out_xsc)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor(xc);
	struct d3d11_service_system *sys = c->sys;

	if (image_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		U_LOG_E("Too many images: %u > %u", image_count, XRT_MAX_SWAPCHAIN_IMAGES);
		return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
	}

	// Allocate swapchain
	struct d3d11_service_swapchain *sc = new d3d11_service_swapchain();
	std::memset(sc, 0, sizeof(*sc));

	sc->base.destroy = swapchain_destroy;
	sc->base.acquire_image = swapchain_acquire_image;
	sc->base.inc_image_use = swapchain_inc_image_use;
	sc->base.dec_image_use = swapchain_dec_image_use;
	sc->base.wait_image = swapchain_wait_image;
	sc->base.barrier_image = swapchain_barrier_image;
	sc->base.release_image = swapchain_release_image;
	sc->base.reference.count = 1;

	sc->comp = c;
	sc->image_count = image_count;
	sc->info = *info;

	U_LOG_D("Importing swapchain: %u images, %ux%u, format=%u, usage=0x%x",
	        image_count, info->width, info->height, info->format, info->bits);

	// Import each image from the client
	for (uint32_t i = 0; i < image_count; i++) {
		HANDLE handle = native_images[i].handle;

		if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
			U_LOG_E("Invalid handle for image [%u]: %p", i, handle);
			swapchain_destroy(&sc->base);
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
		HRESULT hr;
		if (is_dxgi) {
			// DXGI shared handle (can work cross-process with AppContainer)
			hr = sys->device->OpenSharedResource1(handle, IID_PPV_ARGS(sc->images[i].texture.put()));
			if (FAILED(hr)) {
				U_LOG_E("OpenSharedResource1 failed for image [%u]: 0x%08lx (handle=%p)",
				        i, hr, handle);
			}
		} else {
			// Legacy NT handle
			hr = sys->device->OpenSharedResource(handle, IID_PPV_ARGS(sc->images[i].texture.put()));
			if (FAILED(hr)) {
				U_LOG_E("OpenSharedResource failed for image [%u]: 0x%08lx (handle=%p)",
				        i, hr, handle);
			}
		}

		if (FAILED(hr)) {
			// Log additional diagnostic information
			U_LOG_E("  Swapchain info: %ux%u, format=%u", info->width, info->height, info->format);
			swapchain_destroy(&sc->base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}

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
			swapchain_destroy(&sc->base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}
	}

	U_LOG_I("Imported swapchain with %u images (%ux%u)", image_count, info->width, info->height);

	*out_xsc = &sc->base;
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
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	U_LOG_I("D3D11 service compositor: session begin");
	return XRT_SUCCESS;
}

static xrt_result_t
compositor_end_session(struct xrt_compositor *xc)
{
	U_LOG_I("D3D11 service compositor: session end");
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
	struct d3d11_service_compositor *c = d3d11_service_compositor(xc);

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
	struct d3d11_service_compositor *c = d3d11_service_compositor(xc);

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
	struct d3d11_service_compositor *c = d3d11_service_compositor(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	c->layer_accum.layer_count = 0;

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	c->layer_accum.layer_count = 0;

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor(xc);

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
	struct d3d11_service_compositor *c = d3d11_service_compositor(xc);

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
	struct d3d11_service_compositor *c = d3d11_service_compositor(xc);

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
	struct d3d11_service_compositor *c = d3d11_service_compositor(xc);

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
	struct d3d11_service_compositor *c = d3d11_service_compositor(xc);

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
	struct d3d11_service_compositor *c = d3d11_service_compositor(xc);

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
	struct d3d11_service_compositor *c = d3d11_service_compositor(xc);

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
	struct d3d11_service_compositor *c = d3d11_service_compositor(xc);

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
	struct d3d11_service_compositor *c = d3d11_service_compositor(xc);
	struct d3d11_service_system *sys = c->sys;

	std::lock_guard<std::mutex> lock(c->mutex);

	// Clear stereo render target
	if (sys->stereo_rtv) {
		float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		sys->context->ClearRenderTargetView(sys->stereo_rtv.get(), clear_color);
	}

	// Render projection layers to stereo texture
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

		struct d3d11_service_swapchain *sc_left = d3d11_service_swapchain(xsc_left);
		struct d3d11_service_swapchain *sc_right = d3d11_service_swapchain(xsc_right);

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
		left_box.left = layer->data.proj.v[0].sub.rect.offset.x;
		left_box.top = layer->data.proj.v[0].sub.rect.offset.y;
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
		right_box.left = layer->data.proj.v[1].sub.rect.offset.x;
		right_box.top = layer->data.proj.v[1].sub.rect.offset.y;
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

#ifdef XRT_HAVE_LEIA_SR
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

		// Set viewport
		D3D11_VIEWPORT viewport = {};
		viewport.Width = static_cast<float>(sys->display_width);
		viewport.Height = static_cast<float>(sys->display_height);
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
	struct d3d11_service_compositor *c = d3d11_service_compositor(xc);
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
	struct d3d11_service_system *sys = d3d11_service_system(xsysc);

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
	uint32_t format_count = 0;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_R8G8B8A8_UNORM;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_B8G8R8A8_UNORM;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	c->base.base.info.format_count = format_count;

	U_LOG_I("D3D11 service: created native compositor for client");

	*out_xcn = &c->base;
	return XRT_SUCCESS;
}

static void
system_destroy(struct xrt_system_compositor *xsysc)
{
	struct d3d11_service_system *sys = d3d11_service_system(xsysc);

	U_LOG_I("Destroying D3D11 service system compositor");

#ifdef XRT_HAVE_LEIA_SR
	if (sys->weaver != nullptr) {
		leiasr_d3d11_destroy(&sys->weaver);
	}
#endif

	if (sys->msc != nullptr) {
		comp_multi_destroy(&sys->msc);
	}

	sys->back_buffer_rtv.reset();
	sys->stereo_rtv.reset();
	sys->stereo_srv.reset();
	sys->stereo_texture.reset();
	sys->swap_chain.reset();
	sys->dxgi_factory.reset();
	sys->context.reset();
	sys->device.reset();

	if (sys->hwnd != nullptr) {
		DestroyWindow(sys->hwnd);
	}

	delete sys;
}


/*
 *
 * Window creation helper
 *
 */

static LRESULT CALLBACK
service_window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg) {
	case WM_CLOSE:
		// Don't close - service manages this
		return 0;
	default:
		return DefWindowProc(hwnd, msg, wparam, lparam);
	}
}

static HWND
create_service_window(uint32_t width, uint32_t height)
{
	const wchar_t *class_name = L"MonadoD3D11ServiceWindow";

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = service_window_proc;
	wc.hInstance = GetModuleHandle(nullptr);
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = class_name;

	RegisterClassExW(&wc);

	// Use WS_EX_NOACTIVATE so service window doesn't steal focus
	HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE,
	                            class_name,
	                            L"Monado D3D11 Service",
	                            WS_OVERLAPPEDWINDOW,
	                            CW_USEDEFAULT, CW_USEDEFAULT,
	                            static_cast<int>(width), static_cast<int>(height),
	                            nullptr, nullptr,
	                            GetModuleHandle(nullptr),
	                            nullptr);

	if (hwnd != nullptr) {
		ShowWindow(hwnd, SW_SHOWNOACTIVATE);
		UpdateWindow(hwnd);
	}

	return hwnd;
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
	U_LOG_I("Creating D3D11 service system compositor");

	// Allocate system compositor
	struct d3d11_service_system *sys = new d3d11_service_system();
	std::memset(&sys->base, 0, sizeof(sys->base));

	sys->xdev = xdev;
	sys->log_level = U_LOGGING_INFO;
	sys->display_width = 1920;
	sys->display_height = 1080;
	sys->view_width = sys->display_width / 2;
	sys->view_height = sys->display_height;
	sys->refresh_rate = 60.0f;

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
	hr = device_base.query_to(sys->device.put());
	if (FAILED(hr)) {
		U_LOG_E("Device doesn't support ID3D11Device5: 0x%08lx", hr);
		delete sys;
		return XRT_ERROR_VULKAN;
	}

	hr = context_base.query_to(sys->context.put());
	if (FAILED(hr)) {
		U_LOG_E("Context doesn't support ID3D11DeviceContext4: 0x%08lx", hr);
		delete sys;
		return XRT_ERROR_VULKAN;
	}

	// Get DXGI factory
	wil::com_ptr<IDXGIDevice> dxgi_device;
	sys->device.query_to(dxgi_device.put());

	wil::com_ptr<IDXGIAdapter> adapter;
	dxgi_device->GetAdapter(adapter.put());

	hr = adapter->GetParent(IID_PPV_ARGS(sys->dxgi_factory.put()));
	if (FAILED(hr)) {
		U_LOG_E("Failed to get DXGI factory: 0x%08lx", hr);
		delete sys;
		return XRT_ERROR_VULKAN;
	}

	// Create service window for display output
	sys->hwnd = create_service_window(sys->display_width, sys->display_height);
	if (sys->hwnd == nullptr) {
		U_LOG_E("Failed to create service window");
		delete sys;
		return XRT_ERROR_VULKAN;
	}

	// Create swap chain
	DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
	sc_desc.Width = sys->display_width;
	sc_desc.Height = sys->display_height;
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

	U_LOG_I("Created stereo render target (%ux%u)", sys->display_width, sys->display_height);

#ifdef XRT_HAVE_LEIA_SR
	// Create SR weaver
	xrt_result_t xret = leiasr_d3d11_create(
	    5.0,  // 5 second timeout
	    sys->device.get(),
	    sys->context.get(),
	    sys->hwnd,
	    sys->view_width,
	    sys->view_height,
	    &sys->weaver);

	if (xret != XRT_SUCCESS) {
		U_LOG_W("Failed to create SR weaver, continuing without interlacing");
		sys->weaver = nullptr;
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
	sys->base.info.compositor_vk_deviceUUID_valid = false;  // No Vulkan
	sys->base.info.is_service_mode = true;

	// Create multi-compositor for managing multiple clients
	xrt_result_t xret = comp_multi_create_system_compositor(
	    &sys->base,
	    nullptr,  // No special settings
	    0, false, // Don't use scratch images
	    &sys->msc);

	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to create multi-compositor: %d", xret);
		system_destroy(&sys->base);
		return xret;
	}

	U_LOG_I("D3D11 service system compositor created (%ux%u @ %.0fHz)",
	        sys->display_width, sys->display_height, sys->refresh_rate);

	*out_xsysc = comp_multi_system_compositor(&sys->msc);
	return XRT_SUCCESS;
}
