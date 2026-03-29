// Copyright 2019-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 client side glue to compositor implementation.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Fernando Velazquez Innella <finnella@magicleap.com>
 * @ingroup comp_client
 */

#include "comp_d3d11_client.h"

#include "comp_d3d_common.hpp"

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_config_os.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_deleters.hpp"
#include "xrt/xrt_results.h"
#include "xrt/xrt_vulkan_includes.h"
#include "d3d/d3d_dxgi_formats.h"
#include "d3d/d3d_d3d11_helpers.hpp"
#include "d3d/d3d_d3d11_allocator.hpp"
#include "d3d/d3d_d3d11_fence.hpp"
#include "util/u_misc.h"
#include "util/u_pretty_print.h"
#include "util/u_time.h"
#include "util/u_logging.h"
#include "util/u_debug.h"
#include "util/u_handles.h"
#include "util/u_win32_com_guard.hpp"

#include <d3d11_1.h>
#include <d3d11_3.h>
#include <wil/resource.h>
#include <wil/com.h>
#include <wil/result_macros.h>

#include <assert.h>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>

using namespace std::chrono_literals;
using namespace std::chrono;

using xrt::compositor::client::unique_swapchain_ref;

DEBUG_GET_ONCE_LOG_OPTION(log, "D3D_COMPOSITOR_LOG", U_LOGGING_INFO)

/*!
 * Spew level logging.
 *
 * @relates client_d3d11_compositor
 */
#define D3D_SPEW(c, ...) U_LOG_IFL_T(c->log_level, __VA_ARGS__);

/*!
 * Debug level logging.
 *
 * @relates client_d3d11_compositor
 */
#define D3D_DEBUG(c, ...) U_LOG_IFL_D(c->log_level, __VA_ARGS__);

/*!
 * Info level logging.
 *
 * @relates client_d3d11_compositor
 */
#define D3D_INFO(c, ...) U_LOG_IFL_I(c->log_level, __VA_ARGS__);

/*!
 * Warn level logging.
 *
 * @relates client_d3d11_compositor
 */
#define D3D_WARN(c, ...) U_LOG_IFL_W(c->log_level, __VA_ARGS__);

/*!
 * Error level logging.
 *
 * @relates client_d3d11_compositor
 */
#define D3D_ERROR(c, ...) U_LOG_IFL_E(c->log_level, __VA_ARGS__);

using unique_compositor_semaphore_ref = std::unique_ptr<
    struct xrt_compositor_semaphore,
    xrt::deleters::reference_deleter<struct xrt_compositor_semaphore, xrt_compositor_semaphore_reference>>;

// 0 is special
static constexpr uint64_t kKeyedMutexKey = 0;

// Timeout to wait for completion
static constexpr auto kFenceTimeout = 500ms;

/*!
 * @class client_d3d11_compositor
 *
 * Wraps the real compositor providing a D3D11 based interface.
 *
 * @ingroup comp_client
 * @implements xrt_compositor_d3d11
 */
struct client_d3d11_compositor
{
	struct xrt_compositor_d3d11 base = {};

	//! Owning reference to the backing native compositor
	struct xrt_compositor_native *xcn{nullptr};

	//! Just keeps COM alive while we keep references to COM things.
	xrt::auxiliary::util::ComGuard com_guard;

	//! Logging level.
	enum u_logging_level log_level = U_LOGGING_INFO;

	//! Device we got from the app
	wil::com_ptr<ID3D11Device5> app_device;

	//! Immediate context for @ref app_device
	wil::com_ptr<ID3D11DeviceContext3> app_context;

	//! A similar device we created on the same adapter
	wil::com_ptr<ID3D11Device5> comp_device;

	//! Immediate context for @ref comp_device
	wil::com_ptr<ID3D11DeviceContext4> comp_context;

	//! Device used for the fence, currently the @ref app_device.
	wil::com_ptr<ID3D11Device5> fence_device;

	//! Immediate context for @ref fence_device
	wil::com_ptr<ID3D11DeviceContext4> fence_context;

	// wil::unique_handle timeline_semaphore_handle;

	/*!
	 * A timeline semaphore made by the native compositor and imported by us.
	 *
	 * When this is valid, we should use xrt_compositor::layer_commit_with_semaphone:
	 * it means the native compositor knows about timeline semaphores, and we can import its semaphores, so we can
	 * pass @ref timeline_semaphore instead of blocking locally.
	 */
	unique_compositor_semaphore_ref timeline_semaphore;

	/*!
	 * A fence (timeline semaphore) object, owned by @ref fence_device.
	 *
	 * Signal using @ref fence_context if this is not null.
	 *
	 * Wait on it in `layer_commit` if @ref timeline_semaphore *is* null/invalid.
	 */
	wil::com_ptr<ID3D11Fence> fence;

	/*!
	 * Event used for blocking in `layer_commit` if required (if @ref timeline_semaphore *is* null/invalid)
	 */
	wil::unique_event_nothrow local_wait_event;

	/*!
	 * The value most recently signaled on the timeline semaphore
	 */
	uint64_t timeline_semaphore_value = 0;
};

static_assert(std::is_standard_layout<client_d3d11_compositor>::value);

struct client_d3d11_swapchain;

static inline DWORD
convertTimeoutToWindowsMilliseconds(int64_t timeout_ns)
{
	return (timeout_ns == XRT_INFINITE_DURATION) ? INFINITE : (DWORD)(timeout_ns / (int64_t)U_TIME_1MS_IN_NS);
}

/*!
 * Split out from @ref client_d3d11_swapchain to ensure that it is standard
 * layout, std::vector for instance is not standard layout.
 */
struct client_d3d11_swapchain_data
{
	explicit client_d3d11_swapchain_data(enum u_logging_level log_level) : keyed_mutex_collection(log_level) {}

	xrt::compositor::client::KeyedMutexCollection keyed_mutex_collection;

	//! The shared DXGI handles for our images
	std::vector<HANDLE> dxgi_handles;

	//! Images associated with client_d3d11_compositor::app_device
	std::vector<wil::com_ptr<ID3D11Texture2D1>> app_images;

	//! Images associated with client_d3d11_compositor::comp_device
	std::vector<wil::com_ptr<ID3D11Texture2D1>> comp_images;
};

/*!
 * Wraps the real compositor swapchain providing a D3D11 based interface.
 *
 * @ingroup comp_client
 * @implements xrt_swapchain_d3d11
 */
struct client_d3d11_swapchain
{
	struct xrt_swapchain_d3d11 base;

	//! Owning reference to the imported swapchain.
	unique_swapchain_ref xsc;

	//! Non-owning reference to our parent compositor.
	struct client_d3d11_compositor *c;

	//! implementation struct with things that aren't standard_layout
	std::unique_ptr<client_d3d11_swapchain_data> data;
};

static_assert(std::is_standard_layout<client_d3d11_swapchain>::value);

/*!
 * Down-cast helper.
 * @private @memberof client_d3d11_swapchain
 */
static inline struct client_d3d11_swapchain *
as_client_d3d11_swapchain(struct xrt_swapchain *xsc)
{
	return reinterpret_cast<client_d3d11_swapchain *>(xsc);
}

/*!
 * Down-cast helper.
 * @private @memberof client_d3d11_compositor
 */
static inline struct client_d3d11_compositor *
as_client_d3d11_compositor(struct xrt_compositor *xc)
{
	return (struct client_d3d11_compositor *)xc;
}


/*
 *
 * Logging helper.
 *
 */
static constexpr size_t kErrorBufSize = 256;

template <size_t N>
static inline bool
formatMessage(DWORD err, char (&buf)[N])
{
	if (0 != FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err,
	                        LANG_SYSTEM_DEFAULT, buf, N - 1, NULL)) {
		return true;
	}
	memset(buf, 0, N);
	return false;
}


/*
 *
 * Swapchain functions.
 *
 */

static xrt_result_t
client_d3d11_swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	struct client_d3d11_swapchain *sc = as_client_d3d11_swapchain(xsc);

	OutputDebugStringA("[DisplayXR] xrAcquireSwapchainImage: ENTER\n");

	// Pipe down call into imported swapchain in native compositor.
	xrt_result_t xret = xrt_swapchain_acquire_image(sc->xsc.get(), out_index);

	char buf[128];
	snprintf(buf, sizeof(buf), "[DisplayXR] xrAcquireSwapchainImage: result=%d, index=%u\n",
	         (int)xret, out_index ? *out_index : 0xFFFFFFFF);
	OutputDebugStringA(buf);

	return xret;
}

static xrt_result_t
client_d3d11_swapchain_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	struct client_d3d11_swapchain *sc = as_client_d3d11_swapchain(xsc);

	char buf[256];
	snprintf(buf, sizeof(buf), "[DisplayXR] xrWaitSwapchainImage: ENTER index=%u, timeout=%lld ns\n",
	         index, (long long)timeout_ns);
	OutputDebugStringA(buf);

	// Pipe down call into imported swapchain in native compositor.
	xrt_result_t xret = xrt_swapchain_wait_image(sc->xsc.get(), timeout_ns, index);

	snprintf(buf, sizeof(buf), "[DisplayXR] xrWaitSwapchainImage: IPC wait result=%d\n", (int)xret);
	OutputDebugStringA(buf);

	if (xret == XRT_SUCCESS) {
		// OK, we got the image in the native compositor, now need the keyed mutex in d3d11.
		OutputDebugStringA("[DisplayXR] xrWaitSwapchainImage: Acquiring KeyedMutex...\n");
		xret = sc->data->keyed_mutex_collection.waitKeyedMutex(index, timeout_ns);
		snprintf(buf, sizeof(buf), "[DisplayXR] xrWaitSwapchainImage: KeyedMutex acquire result=%d\n", (int)xret);
		OutputDebugStringA(buf);
	}

	//! @todo discard old contents?
	return xret;
}

static xrt_result_t
client_d3d11_swapchain_barrier_image(struct xrt_swapchain *xsc, enum xrt_barrier_direction direction, uint32_t index)
{
	return XRT_SUCCESS;
}

static xrt_result_t
client_d3d11_swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	struct client_d3d11_swapchain *sc = as_client_d3d11_swapchain(xsc);

	char buf[128];
	snprintf(buf, sizeof(buf), "[DisplayXR] xrReleaseSwapchainImage: ENTER index=%u\n", index);
	OutputDebugStringA(buf);

	// Pipe down call into imported swapchain in native compositor.
	xrt_result_t xret = xrt_swapchain_release_image(sc->xsc.get(), index);

	snprintf(buf, sizeof(buf), "[DisplayXR] xrReleaseSwapchainImage: IPC release result=%d\n", (int)xret);
	OutputDebugStringA(buf);

	if (xret == XRT_SUCCESS) {
		// Release the keyed mutex
		OutputDebugStringA("[DisplayXR] xrReleaseSwapchainImage: Releasing KeyedMutex...\n");
		xret = sc->data->keyed_mutex_collection.releaseKeyedMutex(index);
		snprintf(buf, sizeof(buf), "[DisplayXR] xrReleaseSwapchainImage: KeyedMutex release result=%d\n", (int)xret);
		OutputDebugStringA(buf);
	}
	return xret;
}

static void
client_d3d11_swapchain_destroy(struct xrt_swapchain *xsc)
{
	// letting destruction do it all
	std::unique_ptr<client_d3d11_swapchain> sc(as_client_d3d11_swapchain(xsc));
}

/*
 *
 * Import helpers
 *
 */

static wil::com_ptr<ID3D11Texture2D1>
import_image(ID3D11Device1 &device, HANDLE h)
{
	wil::com_ptr<ID3D11Texture2D1> tex;

	if (h == nullptr) {
		OutputDebugStringA("[DisplayXR] import_image: handle is NULL!\n");
		return {};
	}

	// Use OpenSharedResource1 for NT handles with detailed HRESULT logging
	HRESULT hr = device.OpenSharedResource1(h, __uuidof(ID3D11Texture2D1), tex.put_void());
	if (FAILED(hr)) {
		char buf[512];
		snprintf(buf, sizeof(buf),
		         "[DisplayXR] OpenSharedResource1 FAILED: handle=%p, HRESULT=0x%08lx\n"
		         "  Common causes:\n"
		         "  - 0x80070057 (E_INVALIDARG): wrong handle type or invalid handle\n"
		         "  - 0x80070005 (E_ACCESSDENIED): security/permissions issue (AppContainer?)\n"
		         "  - 0x887a0005 (DXGI_ERROR_DEVICE_REMOVED): wrong GPU adapter (LUID mismatch)\n"
		         "  - 0x887a0006 (DXGI_ERROR_DEVICE_HUNG): GPU hung during operation\n",
		         h, (unsigned long)hr);
		OutputDebugStringA(buf);
		THROW_IF_FAILED(hr);  // Re-throw for proper error propagation
	}

	char buf[128];
	snprintf(buf, sizeof(buf), "[DisplayXR] OpenSharedResource1 SUCCESS: handle=%p -> texture=%p\n",
	         h, (void*)tex.get());
	OutputDebugStringA(buf);
	return tex;
}

static wil::com_ptr<ID3D11Texture2D1>
import_image_dxgi(ID3D11Device1 &device, HANDLE h)
{
	wil::com_ptr<ID3D11Texture2D1> tex;

	if (h == nullptr) {
		OutputDebugStringA("[DisplayXR] import_image_dxgi: handle is NULL!\n");
		return {};
	}

	// Use OpenSharedResource for legacy DXGI handles with detailed HRESULT logging
	HRESULT hr = device.OpenSharedResource(h, __uuidof(ID3D11Texture2D1), tex.put_void());
	if (FAILED(hr)) {
		char buf[512];
		snprintf(buf, sizeof(buf),
		         "[DisplayXR] OpenSharedResource FAILED: handle=%p, HRESULT=0x%08lx\n"
		         "  Common causes:\n"
		         "  - 0x80070057 (E_INVALIDARG): wrong handle type (expected DXGI, got NT?)\n"
		         "  - 0x80070005 (E_ACCESSDENIED): security/permissions issue\n"
		         "  - 0x887a0005 (DXGI_ERROR_DEVICE_REMOVED): wrong GPU adapter\n",
		         h, (unsigned long)hr);
		OutputDebugStringA(buf);
		THROW_IF_FAILED(hr);  // Re-throw for proper error propagation
	}

	char buf[128];
	snprintf(buf, sizeof(buf), "[DisplayXR] OpenSharedResource SUCCESS: handle=%p -> texture=%p\n",
	         h, (void*)tex.get());
	OutputDebugStringA(buf);
	return tex;
}

static wil::com_ptr<ID3D11Fence>
import_fence(ID3D11Device5 &device, HANDLE h)
{
	wil::com_ptr<ID3D11Fence> fence;

	if (h == nullptr) {
		return {};
	}
	THROW_IF_FAILED(device.OpenSharedFence(h, __uuidof(ID3D11Fence), fence.put_void()));
	return fence;
}


xrt_result_t
client_d3d11_create_swapchain(struct xrt_compositor *xc,
                              const struct xrt_swapchain_create_info *info,
                              struct xrt_swapchain **out_xsc)
try {
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);
	xrt_result_t xret = XRT_SUCCESS;
	xrt_swapchain_create_properties xsccp{};
	xret = xrt_comp_get_swapchain_create_properties(xc, info, &xsccp);

	if (xret != XRT_SUCCESS) {
		D3D_ERROR(c, "Could not get properties for creating swapchain");
		return xret;
	}
	uint32_t image_count = xsccp.image_count;


	if ((info->create & XRT_SWAPCHAIN_CREATE_PROTECTED_CONTENT) != 0) {
		D3D_WARN(c,
		         "Swapchain info is valid but this compositor doesn't support creating protected content "
		         "swapchains!");
		return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
	}

	int64_t vk_format = d3d_dxgi_format_to_vk((DXGI_FORMAT)info->format);
	if (vk_format == 0) {
		D3D_ERROR(c, "Invalid format!");
		return XRT_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}

	struct xrt_swapchain_create_info vkinfo = *info;

	// Update the create info with Vulkan format for IPC to the service.
	vkinfo.format = vk_format;
	vkinfo.bits = (enum xrt_swapchain_usage_bits)(xsccp.extra_bits | vkinfo.bits);

	std::unique_ptr<struct client_d3d11_swapchain> sc = std::make_unique<struct client_d3d11_swapchain>();
	sc->data = std::make_unique<client_d3d11_swapchain_data>(c->log_level);
	auto &data = sc->data;

	// Ask the service to create the swapchain (server-creates-swapchain model).
	// This is required for WebXR support because Chrome's sandboxed GPU process
	// cannot export D3D11 shared handles.
	D3D_INFO(c, "Requesting server to create swapchain (server-creates-swapchain model)");
	xrt_swapchain *xsc_raw = nullptr;
	xret = xrt_comp_create_swapchain(&c->xcn->base, &vkinfo, &xsc_raw);
	if (xret != XRT_SUCCESS) {
		D3D_ERROR(c, "Service failed to create swapchain: %d", xret);
		return xret;
	}
	// Transfer ownership to the unique_ptr
	sc->xsc.reset(xsc_raw);

	// Get handles from service-created swapchain
	struct xrt_swapchain_native *xscn = (struct xrt_swapchain_native *)sc->xsc.get();
	image_count = xscn->base.image_count;
	D3D_INFO(c, "Service created swapchain with %u images", image_count);

	data->app_images.reserve(image_count);

	// Import server-provided handles into app_device
	for (uint32_t i = 0; i < image_count; ++i) {
		HANDLE handle = (HANDLE)xscn->images[i].handle;
		bool is_dxgi = xscn->images[i].is_dxgi_handle;

		// Clear DXGI marker bit if set (handles are marked with bit 0 during IPC transfer)
		if ((size_t)handle & 1) {
			handle = (HANDLE)((size_t)handle & ~1);
			is_dxgi = true;
		}

		data->dxgi_handles.push_back(handle);

		D3D_DEBUG(c, "Importing server texture [%u]: handle=%p, is_dxgi=%d", i, handle, is_dxgi);
		{
			char dbg_buf[256];
			snprintf(dbg_buf, sizeof(dbg_buf),
			         "[DisplayXR] Importing texture [%u]: handle=%p, is_dxgi=%d\n",
			         i, handle, is_dxgi);
			OutputDebugStringA(dbg_buf);
		}

		wil::com_ptr<ID3D11Texture2D1> image;
		try {
			if (is_dxgi) {
				// Legacy DXGI global handle - use OpenSharedResource
				image = import_image_dxgi(*(c->app_device), handle);
			} else {
				// NT handle - use OpenSharedResource1
				image = import_image(*(c->app_device), handle);
			}
		} catch (wil::ResultException const &e) {
			// Extract HRESULT and log detailed error
			char dbg_buf[512];
			snprintf(dbg_buf, sizeof(dbg_buf),
			         "[DisplayXR] IMPORT FAILED [%u]: handle=%p, is_dxgi=%d, error=%s\n",
			         i, handle, is_dxgi, e.what());
			OutputDebugStringA(dbg_buf);
			D3D_ERROR(c, "Failed to import texture [%u]: %s", i, e.what());
			D3D_ERROR(c, "  Handle=%p, is_dxgi=%d", handle, is_dxgi);
			D3D_ERROR(c, "  Common causes: adapter LUID mismatch, invalid handle, security descriptor");
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}
		if (!image) {
			char dbg_buf[256];
			snprintf(dbg_buf, sizeof(dbg_buf),
			         "[DisplayXR] IMPORT RETURNED NULL [%u]: handle=%p, is_dxgi=%d\n",
			         i, handle, is_dxgi);
			OutputDebugStringA(dbg_buf);
			D3D_ERROR(c, "Failed to import server texture [%u] with handle %p (is_dxgi=%d)", i, handle, is_dxgi);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}
		{
			char dbg_buf[256];
			snprintf(dbg_buf, sizeof(dbg_buf),
			         "[DisplayXR] Import SUCCESS [%u]: texture=%p\n", i, (void*)image.get());
			OutputDebugStringA(dbg_buf);
		}

		// Put the image where the OpenXR state tracker can get it
		sc->base.images[i] = image.get();

		// Store the owning pointer for lifetime management
		data->app_images.emplace_back(std::move(image));
	}

	// Cache the keyed mutex interface
	xret = data->keyed_mutex_collection.init(data->app_images);
	if (xret != XRT_SUCCESS) {
		D3D_ERROR(c, "Error retrieving keyed mutex interfaces");
		return xret;
	}

	sc->base.base.destroy = client_d3d11_swapchain_destroy;
	sc->base.base.acquire_image = client_d3d11_swapchain_acquire_image;
	sc->base.base.wait_image = client_d3d11_swapchain_wait_image;
	sc->base.base.barrier_image = client_d3d11_swapchain_barrier_image;
	sc->base.base.release_image = client_d3d11_swapchain_release_image;
	sc->c = c;
	sc->base.base.image_count = image_count;

	xrt_swapchain_reference(out_xsc, &sc->base.base);
	(void)sc.release();
	return XRT_SUCCESS;
} catch (wil::ResultException const &e) {
	U_LOG_E("Error creating D3D11 swapchain: %s", e.what());
	return XRT_ERROR_ALLOCATION;
} catch (std::exception const &e) {
	U_LOG_E("Error creating D3D11 swapchain: %s", e.what());
	return XRT_ERROR_ALLOCATION;
} catch (...) {
	U_LOG_E("Error creating D3D11 swapchain");
	return XRT_ERROR_ALLOCATION;
}

static xrt_result_t
client_d3d11_compositor_passthrough_create(struct xrt_compositor *xc, const struct xrt_passthrough_create_info *info)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	// Pipe down call into native compositor.
	return xrt_comp_create_passthrough(&c->xcn->base, info);
}

static xrt_result_t
client_d3d11_compositor_passthrough_layer_create(struct xrt_compositor *xc,
                                                 const struct xrt_passthrough_layer_create_info *info)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	// Pipe down call into native compositor.
	return xrt_comp_create_passthrough_layer(&c->xcn->base, info);
}

static xrt_result_t
client_d3d11_compositor_passthrough_destroy(struct xrt_compositor *xc)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	// Pipe down call into native compositor.
	return xrt_comp_destroy_passthrough(&c->xcn->base);
}

/*
 *
 * Compositor functions.
 *
 */


static xrt_result_t
client_d3d11_compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	// Pipe down call into native compositor.
	return xrt_comp_begin_session(&c->xcn->base, info);
}

static xrt_result_t
client_d3d11_compositor_end_session(struct xrt_compositor *xc)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	// Pipe down call into native compositor.
	return xrt_comp_end_session(&c->xcn->base);
}

static xrt_result_t
client_d3d11_compositor_wait_frame(struct xrt_compositor *xc,
                                   int64_t *out_frame_id,
                                   int64_t *predicted_display_time,
                                   int64_t *predicted_display_period)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	OutputDebugStringA("[DisplayXR] xrWaitFrame: ENTER\n");

	// Pipe down call into native compositor.
	xrt_result_t xret = xrt_comp_wait_frame(&c->xcn->base, out_frame_id, predicted_display_time, predicted_display_period);

	char buf[256];
	snprintf(buf, sizeof(buf), "[DisplayXR] xrWaitFrame: result=%d, frame_id=%lld\n",
	         (int)xret, out_frame_id ? (long long)*out_frame_id : -1);
	OutputDebugStringA(buf);

	return xret;
}

static xrt_result_t
client_d3d11_compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	char buf[128];
	snprintf(buf, sizeof(buf), "[DisplayXR] xrBeginFrame: ENTER frame_id=%lld\n", (long long)frame_id);
	OutputDebugStringA(buf);

	// Pipe down call into native compositor.
	xrt_result_t xret = xrt_comp_begin_frame(&c->xcn->base, frame_id);

	snprintf(buf, sizeof(buf), "[DisplayXR] xrBeginFrame: result=%d\n", (int)xret);
	OutputDebugStringA(buf);

	return xret;
}

static xrt_result_t
client_d3d11_compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	// Pipe down call into native compositor.
	return xrt_comp_discard_frame(&c->xcn->base, frame_id);
}

static xrt_result_t
client_d3d11_compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	// Pipe down call into native compositor.
	return xrt_comp_layer_begin(&c->xcn->base, data);
}

static xrt_result_t
client_d3d11_compositor_layer_projection(struct xrt_compositor *xc,
                                         struct xrt_device *xdev,
                                         struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                         const struct xrt_layer_data *data)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	assert(data->type == XRT_LAYER_PROJECTION);

	struct xrt_swapchain *xscn[XRT_MAX_VIEWS];
	for (uint32_t i = 0; i < data->view_count; ++i) {
		xscn[i] = as_client_d3d11_swapchain(xsc[i])->xsc.get();
	}

	// No flip required: D3D11 swapchain image convention matches Vulkan.
	return xrt_comp_layer_projection(&c->xcn->base, xdev, xscn, data);
}

static xrt_result_t
client_d3d11_compositor_layer_projection_depth(struct xrt_compositor *xc,
                                               struct xrt_device *xdev,
                                               struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                               struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                               const struct xrt_layer_data *data)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	assert(data->type == XRT_LAYER_PROJECTION_DEPTH);
	struct xrt_swapchain *xscn[XRT_MAX_VIEWS];
	struct xrt_swapchain *d_xscn[XRT_MAX_VIEWS];
	for (uint32_t i = 0; i < data->view_count; ++i) {
		xscn[i] = as_client_d3d11_swapchain(xsc[i])->xsc.get();
		d_xscn[i] = as_client_d3d11_swapchain(d_xsc[i])->xsc.get();
	}

	// No flip required: D3D11 swapchain image convention matches Vulkan.
	return xrt_comp_layer_projection_depth(&c->xcn->base, xdev, xscn, d_xscn, data);
}

static xrt_result_t
client_d3d11_compositor_layer_quad(struct xrt_compositor *xc,
                                   struct xrt_device *xdev,
                                   struct xrt_swapchain *xsc,
                                   const struct xrt_layer_data *data)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	assert(data->type == XRT_LAYER_QUAD);

	struct xrt_swapchain *xscfb = as_client_d3d11_swapchain(xsc)->xsc.get();

	// No flip required: D3D11 swapchain image convention matches Vulkan.
	return xrt_comp_layer_quad(&c->xcn->base, xdev, xscfb, data);
}

static xrt_result_t
client_d3d11_compositor_layer_cube(struct xrt_compositor *xc,
                                   struct xrt_device *xdev,
                                   struct xrt_swapchain *xsc,
                                   const struct xrt_layer_data *data)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	assert(data->type == XRT_LAYER_CUBE);

	struct xrt_swapchain *xscfb = as_client_d3d11_swapchain(xsc)->xsc.get();

	// No flip required: D3D11 swapchain image convention matches Vulkan.
	return xrt_comp_layer_cube(&c->xcn->base, xdev, xscfb, data);
}

static xrt_result_t
client_d3d11_compositor_layer_cylinder(struct xrt_compositor *xc,
                                       struct xrt_device *xdev,
                                       struct xrt_swapchain *xsc,
                                       const struct xrt_layer_data *data)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	assert(data->type == XRT_LAYER_CYLINDER);

	struct xrt_swapchain *xscfb = as_client_d3d11_swapchain(xsc)->xsc.get();

	// No flip required: D3D11 swapchain image convention matches Vulkan.
	return xrt_comp_layer_cylinder(&c->xcn->base, xdev, xscfb, data);
}

static xrt_result_t
client_d3d11_compositor_layer_equirect1(struct xrt_compositor *xc,
                                        struct xrt_device *xdev,
                                        struct xrt_swapchain *xsc,
                                        const struct xrt_layer_data *data)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	assert(data->type == XRT_LAYER_EQUIRECT1);

	struct xrt_swapchain *xscfb = as_client_d3d11_swapchain(xsc)->xsc.get();

	// No flip required: D3D11 swapchain image convention matches Vulkan.
	return xrt_comp_layer_equirect1(&c->xcn->base, xdev, xscfb, data);
}

static xrt_result_t
client_d3d11_compositor_layer_equirect2(struct xrt_compositor *xc,
                                        struct xrt_device *xdev,
                                        struct xrt_swapchain *xsc,
                                        const struct xrt_layer_data *data)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	assert(data->type == XRT_LAYER_EQUIRECT2);

	struct xrt_swapchain *xscfb = as_client_d3d11_swapchain(xsc)->xsc.get();

	// No flip required: D3D11 swapchain image convention matches Vulkan.
	return xrt_comp_layer_equirect2(&c->xcn->base, xdev, xscfb, data);
}

static xrt_result_t
client_d3d11_compositor_layer_passthrough(struct xrt_compositor *xc,
                                          struct xrt_device *xdev,
                                          const struct xrt_layer_data *data)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	assert(data->type == XRT_LAYER_PASSTHROUGH);

	// No flip required: D3D11 swapchain image convention matches Vulkan.
	return xrt_comp_layer_passthrough(&c->xcn->base, xdev, data);
}

static xrt_result_t
client_d3d11_compositor_layer_window_space(struct xrt_compositor *xc,
                                           struct xrt_device *xdev,
                                           struct xrt_swapchain *xsc,
                                           const struct xrt_layer_data *data)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	assert(data->type == XRT_LAYER_WINDOW_SPACE);

	struct xrt_swapchain *xscfb = as_client_d3d11_swapchain(xsc)->xsc.get();

	// No flip required: D3D11 swapchain image convention matches Vulkan.
	return xrt_comp_layer_window_space(&c->xcn->base, xdev, xscfb, data);
}

static xrt_result_t
client_d3d11_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	OutputDebugStringA("[DisplayXR] xrEndFrame (layer_commit): ENTER\n");

	// We make the sync object, not st/oxr which is our user.
	assert(!xrt_graphics_sync_handle_is_valid(sync_handle));

	xrt_result_t xret = XRT_SUCCESS;
	if (c->fence) {
		c->timeline_semaphore_value++;

		{
			static int trace_count = 0;
			if (trace_count < 3) {
				U_LOG_W("D3D11-CLIENT[%d]: about to Signal fence value=%llu",
				        trace_count++, (unsigned long long)c->timeline_semaphore_value);
			}
		}

		HRESULT hr = c->fence_context->Signal(c->fence.get(), c->timeline_semaphore_value);

		{
			static int trace_count = 0;
			if (trace_count < 3) {
				U_LOG_W("D3D11-CLIENT[%d]: fence Signal returned hr=0x%08X",
				        trace_count++, (unsigned)hr);
			}
		}

		if (!SUCCEEDED(hr)) {
			char buf[kErrorBufSize];
			formatMessage(hr, buf);
			D3D_ERROR(c, "Error signaling fence: %s", buf);
			OutputDebugStringA("[DisplayXR] xrEndFrame: FENCE SIGNAL FAILED!\n");
			return xrt_comp_layer_commit(&c->xcn->base, XRT_GRAPHICS_SYNC_HANDLE_INVALID);
		}
		OutputDebugStringA("[DisplayXR] xrEndFrame: Fence signaled OK\n");
	}

	if (c->timeline_semaphore) {
		// We got this from the native compositor, so we can pass it back
		OutputDebugStringA("[DisplayXR] xrEndFrame: Using timeline semaphore commit\n");
		xret = xrt_comp_layer_commit_with_semaphore( //
		    &c->xcn->base,                           //
		    c->timeline_semaphore.get(),             //
		    c->timeline_semaphore_value);            //
		char buf[128];
		snprintf(buf, sizeof(buf), "[DisplayXR] xrEndFrame: commit_with_semaphore result=%d\n", (int)xret);
		OutputDebugStringA(buf);
		return xret;
	}

	if (c->fence) {
		// Wait on it ourselves, if we have it and didn't tell the native compositor to wait on it.
		OutputDebugStringA("[DisplayXR] xrEndFrame: Waiting on fence locally...\n");
		xret = xrt::auxiliary::d3d::d3d11::waitOnFenceWithTimeout( //
		    c->fence,                                              //
		    c->local_wait_event,                                   //
		    c->timeline_semaphore_value,                           //
		    kFenceTimeout);                                        //
		if (xret != XRT_SUCCESS) {
			struct u_pp_sink_stack_only sink; // Not inited, very large.
			u_pp_delegate_t dg = u_pp_sink_stack_only_init(&sink);
			u_pp(dg, "Problem waiting on fence: ");
			u_pp_xrt_result(dg, xret);
			D3D_ERROR(c, "%s", sink.buffer);

			return xret;
		}
	}

	xret = xrt_comp_layer_commit(&c->xcn->base, XRT_GRAPHICS_SYNC_HANDLE_INVALID);
	return xret;
}


static xrt_result_t
client_d3d11_compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                                        const struct xrt_swapchain_create_info *info,
                                                        struct xrt_swapchain_create_properties *xsccp)
{
	struct client_d3d11_compositor *c = as_client_d3d11_compositor(xc);

	int64_t vk_format = d3d_dxgi_format_to_vk((DXGI_FORMAT)info->format);
	if (vk_format == 0) {
		D3D_ERROR(c, "Invalid format!");
		return XRT_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}

	struct xrt_swapchain_create_info xinfo = *info;
	xinfo.format = vk_format;

	return xrt_comp_get_swapchain_create_properties(&c->xcn->base, &xinfo, xsccp);
}

static void
client_d3d11_compositor_destroy(struct xrt_compositor *xc)
{
	std::unique_ptr<struct client_d3d11_compositor> c{as_client_d3d11_compositor(xc)};
}

static void
client_d3d11_compositor_init_try_timeline_semaphores(struct client_d3d11_compositor *c)
{
	c->timeline_semaphore_value = 1;
	// See if we can make a "timeline semaphore", also known as ID3D11Fence
	if (!c->xcn->base.create_semaphore || !c->xcn->base.layer_commit_with_semaphore) {
		return;
	}
	struct xrt_compositor_semaphore *xcsem = nullptr;
	wil::unique_handle timeline_semaphore_handle;
	if (XRT_SUCCESS != xrt_comp_create_semaphore(&(c->xcn->base), timeline_semaphore_handle.put(), &xcsem)) {
		D3D_WARN(c, "Native compositor tried but failed to created a timeline semaphore for us.");
		return;
	}
	D3D_INFO(c, "Native compositor created a timeline semaphore for us.");

	unique_compositor_semaphore_ref timeline_semaphore{xcsem};

	// try to import and signal
	wil::com_ptr<ID3D11Fence> fence = import_fence(*(c->fence_device), timeline_semaphore_handle.get());
	HRESULT hr = c->fence_context->Signal(fence.get(), c->timeline_semaphore_value);
	if (!SUCCEEDED(hr)) {
		D3D_WARN(c,
		         "Your graphics driver does not support importing the native compositor's "
		         "semaphores into D3D11, falling back to local blocking.");
		return;
	}

	D3D_INFO(c, "We imported a timeline semaphore and can signal it.");
	// OK, keep these resources around.
	c->fence = std::move(fence);
	c->timeline_semaphore = std::move(timeline_semaphore);
	// c->timeline_semaphore_handle = std::move(timeline_semaphore_handle);
}

static void
client_d3d11_compositor_init_try_internal_blocking(struct client_d3d11_compositor *c)
{
	wil::com_ptr<ID3D11Fence> fence;
	HRESULT hr = c->fence_device->CreateFence( //
	    0,                                     // InitialValue
	    D3D11_FENCE_FLAG_NONE,                 // Flags
	    __uuidof(ID3D11Fence),                 // ReturnedInterface
	    fence.put_void());                     // ppFence

	if (!SUCCEEDED(hr)) {
		char buf[kErrorBufSize];
		formatMessage(hr, buf);
		D3D_WARN(c, "Cannot even create an ID3D11Fence for internal use: %s", buf);
		return;
	}

	hr = c->local_wait_event.create();
	if (!SUCCEEDED(hr)) {
		char buf[kErrorBufSize];
		formatMessage(hr, buf);
		D3D_ERROR(c, "Error creating event for synchronization usage: %s", buf);
		return;
	}

	D3D_INFO(c, "We created our own ID3D11Fence and will wait on it ourselves.");
	c->fence = std::move(fence);
}

struct xrt_compositor_d3d11 *
client_d3d11_compositor_create(struct xrt_compositor_native *xcn, ID3D11Device *device)
try {
	// Use OutputDebugString for diagnostics - visible in DebugView even in sandboxed processes
	OutputDebugStringA("[DisplayXR] client_d3d11_compositor_create: ENTER\n");

	char dbg_buf[256];
	snprintf(dbg_buf, sizeof(dbg_buf), "[DisplayXR] client_d3d11_compositor_create: xcn=%p, device=%p\n", (void*)xcn, (void*)device);
	OutputDebugStringA(dbg_buf);
	U_LOG_W("client_d3d11_compositor_create: xcn=%p, device=%p", (void*)xcn, (void*)device);

	std::unique_ptr<struct client_d3d11_compositor> c = std::make_unique<struct client_d3d11_compositor>();
	c->log_level = debug_get_log_option_log();
	c->xcn = xcn;

	// Log incoming format info from IPC compositor
	snprintf(dbg_buf, sizeof(dbg_buf), "[DisplayXR] IPC compositor format_count=%u\n", xcn->base.info.format_count);
	OutputDebugStringA(dbg_buf);
	U_LOG_W("client_d3d11_compositor_create: IPC compositor format_count=%u", xcn->base.info.format_count);
	for (uint32_t i = 0; i < xcn->base.info.format_count && i < 8; i++) {
		snprintf(dbg_buf, sizeof(dbg_buf), "[DisplayXR]   format[%u] = %lld (VkFormat)\n", i, (long long)xcn->base.info.formats[i]);
		OutputDebugStringA(dbg_buf);
		U_LOG_W("  format[%u] = 0x%llx (VkFormat)", i, (unsigned long long)xcn->base.info.formats[i]);
	}

	wil::com_ptr<ID3D11Device> app_dev{device};
	if (!app_dev.try_query_to(c->app_device.put())) {
		OutputDebugStringA("[DisplayXR] ERROR: Could not query ID3D11Device5 from app device!\n");
		U_LOG_E("client_d3d11_compositor_create: Could not query ID3D11Device5 from app device!");
		U_LOG_E("  This usually means the D3D11 feature level is too low (need 11_1 or higher)");
		return nullptr;
	}
	OutputDebugStringA("[DisplayXR] Got ID3D11Device5 from app device\n");
	U_LOG_W("client_d3d11_compositor_create: Got ID3D11Device5 from app device");
	c->app_device->GetImmediateContext3(c->app_context.put());

	wil::com_ptr<IDXGIAdapter> adapter;

	THROW_IF_FAILED(app_dev.query<IDXGIDevice>()->GetAdapter(adapter.put()));

	// Log the adapter LUID - this MUST match the service compositor's LUID for cross-process texture sharing
	{
		DXGI_ADAPTER_DESC adapter_desc = {};
		HRESULT hr = adapter->GetDesc(&adapter_desc);
		if (SUCCEEDED(hr)) {
			snprintf(dbg_buf, sizeof(dbg_buf),
			         "[DisplayXR] Client D3D11 adapter LUID: %08lx-%08lx (MUST match service LUID)\n",
			         adapter_desc.AdapterLuid.HighPart, adapter_desc.AdapterLuid.LowPart);
			OutputDebugStringA(dbg_buf);
			U_LOG_W("Client D3D11 adapter LUID: %08lx-%08lx (must match service for texture sharing)",
			        adapter_desc.AdapterLuid.HighPart, adapter_desc.AdapterLuid.LowPart);

			// Also log adapter name for debugging multi-GPU systems
			char adapter_name[128];
			wcstombs(adapter_name, adapter_desc.Description, sizeof(adapter_name) - 1);
			adapter_name[sizeof(adapter_name) - 1] = '\0';
			snprintf(dbg_buf, sizeof(dbg_buf), "[DisplayXR] Client adapter: %s\n", adapter_name);
			OutputDebugStringA(dbg_buf);
			U_LOG_W("Client adapter: %s", adapter_name);
		} else {
			OutputDebugStringA("[DisplayXR] WARNING: Could not get adapter LUID!\n");
			U_LOG_W("Could not get adapter LUID - HRESULT=0x%08lx", (unsigned long)hr);
		}
	}

	{
		// Now, try to get an equivalent device of our own
		wil::com_ptr<ID3D11Device> our_dev;
		wil::com_ptr<ID3D11DeviceContext> our_context;
		std::tie(our_dev, our_context) = xrt::auxiliary::d3d::d3d11::createDevice(adapter, c->log_level);
		our_dev.query_to(c->comp_device.put());
		our_context.query_to(c->comp_context.put());
	}

	// Upcast fence to context version 4 and reference fence device.
	{
		c->app_device.query_to(c->fence_device.put());
		c->app_context.query_to(c->fence_context.put());
	}

	// See if we can make a "timeline semaphore", also known as ID3D11Fence
	client_d3d11_compositor_init_try_timeline_semaphores(c.get());
	if (!c->timeline_semaphore) {
		// OK native compositor doesn't know how to handle timeline semaphores, or we can't import them, but we
		// can still use them entirely internally.
		client_d3d11_compositor_init_try_internal_blocking(c.get());
	}
	if (!c->fence) {
		D3D_WARN(c, "No sync mechanism for D3D11 was successful!");
	}
	c->base.base.get_swapchain_create_properties = client_d3d11_compositor_get_swapchain_create_properties;
	c->base.base.create_swapchain = client_d3d11_create_swapchain;
	c->base.base.create_passthrough = client_d3d11_compositor_passthrough_create;
	c->base.base.create_passthrough_layer = client_d3d11_compositor_passthrough_layer_create;
	c->base.base.destroy_passthrough = client_d3d11_compositor_passthrough_destroy;
	c->base.base.begin_session = client_d3d11_compositor_begin_session;
	c->base.base.end_session = client_d3d11_compositor_end_session;
	c->base.base.wait_frame = client_d3d11_compositor_wait_frame;
	c->base.base.begin_frame = client_d3d11_compositor_begin_frame;
	c->base.base.discard_frame = client_d3d11_compositor_discard_frame;
	c->base.base.layer_begin = client_d3d11_compositor_layer_begin;
	c->base.base.layer_projection = client_d3d11_compositor_layer_projection;
	c->base.base.layer_projection_depth = client_d3d11_compositor_layer_projection_depth;
	c->base.base.layer_quad = client_d3d11_compositor_layer_quad;
	c->base.base.layer_cube = client_d3d11_compositor_layer_cube;
	c->base.base.layer_cylinder = client_d3d11_compositor_layer_cylinder;
	c->base.base.layer_equirect1 = client_d3d11_compositor_layer_equirect1;
	c->base.base.layer_equirect2 = client_d3d11_compositor_layer_equirect2;
	c->base.base.layer_passthrough = client_d3d11_compositor_layer_passthrough;
	c->base.base.layer_window_space = client_d3d11_compositor_layer_window_space;
	c->base.base.layer_commit = client_d3d11_compositor_layer_commit;
	c->base.base.destroy = client_d3d11_compositor_destroy;


	// Passthrough our formats from the native compositor to the client.
	U_LOG_W("client_d3d11_compositor_create: Converting %u formats from IPC compositor", xcn->base.info.format_count);
	uint32_t count = 0;
	for (uint32_t i = 0; i < xcn->base.info.format_count; i++) {
		// Can we turn this format into DXGI?
		DXGI_FORMAT f = d3d_vk_format_to_dxgi(xcn->base.info.formats[i]);
		if (f == 0) {
			U_LOG_D("  format[%u]: VkFormat 0x%llx -> no DXGI equivalent, skipping", i, (unsigned long long)xcn->base.info.formats[i]);
			continue;
		}
		// And back to Vulkan?
		auto v = d3d_dxgi_format_to_vk(f);
		if (v == 0) {
			U_LOG_D("  format[%u]: DXGI %u -> no VkFormat equivalent, skipping", i, (unsigned)f);
			continue;
		}
		// Do we have a typeless version of it?
		DXGI_FORMAT typeless = d3d_dxgi_format_to_typeless_dxgi(f);
		if (typeless == f) {
			U_LOG_D("  format[%u]: DXGI %u -> no typeless version, skipping", i, (unsigned)f);
			continue;
		}

		c->base.base.info.formats[count++] = f;
	}
	c->base.base.info.format_count = count;
	U_LOG_W("client_d3d11_compositor_create: Final format_count=%u", count);

	if (count == 0) {
		OutputDebugStringA("[DisplayXR] ERROR: No compatible DXGI formats found!\n");
		U_LOG_E("client_d3d11_compositor_create: No compatible DXGI formats found!");
		U_LOG_E("  IPC compositor reported %u formats but none are usable for D3D11", xcn->base.info.format_count);
		// Don't return nullptr here - let the caller handle zero formats
	}

	OutputDebugStringA("[DisplayXR] client_d3d11_compositor_create: SUCCESS\n");
	U_LOG_W("client_d3d11_compositor_create: SUCCESS - D3D11 client compositor ready");
	return &(c.release()->base);
} catch (wil::ResultException const &e) {
	char dbg_buf[512];
	snprintf(dbg_buf, sizeof(dbg_buf), "[DisplayXR] WIL EXCEPTION: %s\n", e.what());
	OutputDebugStringA(dbg_buf);
	U_LOG_E("Error creating D3D11 client compositor: %s", e.what());
	return nullptr;
} catch (std::exception const &e) {
	char dbg_buf[512];
	snprintf(dbg_buf, sizeof(dbg_buf), "[DisplayXR] STD EXCEPTION: %s\n", e.what());
	OutputDebugStringA(dbg_buf);
	U_LOG_E("Error creating D3D11 client compositor: %s", e.what());
	return nullptr;
} catch (...) {
	OutputDebugStringA("[DisplayXR] UNKNOWN EXCEPTION in client_d3d11_compositor_create!\n");
	U_LOG_E("Error creating D3D11 client compositor");
	return nullptr;
}
