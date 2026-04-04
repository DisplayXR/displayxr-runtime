// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenGL client swapchain using WGL_NV_DX_interop2 for D3D11 shared textures.
 *
 * When the IPC service is a D3D11 compositor, it exports shared textures as
 * NT handles. This file imports those textures into GL via WGL_NV_DX_interop2,
 * which is supported on NVIDIA (since ~2012) and recent AMD/Intel drivers.
 *
 * Reference: src/xrt/compositor/gl/comp_gl_compositor.c (server-side interop).
 *
 * @ingroup comp_client
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xrt/xrt_config_have.h"
#include "xrt/xrt_windows.h"
#include "xrt/xrt_handles.h"

#include "util/u_misc.h"
#include "util/u_logging.h"

#include "ogl/ogl_api.h"

#include "client/comp_gl_client.h"
#include "client/comp_gl_d3d11_swapchain.h"

#include <d3d11_1.h>

// GUIDs for C-style COM (same approach as comp_gl_compositor.c)
static const IID IID_ID3D11Texture2D_local = {
    0x6f15aaf2, 0xd208, 0x4e89,
    {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};

// IID_ID3D11Device1: {a04bfb29-08ef-43d6-a49c-a9bdbdcbe686}
static const IID IID_ID3D11Device1_local = {
    0xa04bfb29, 0x08ef, 0x43d6,
    {0xa4, 0x9c, 0xa9, 0xbd, 0xbd, 0xcb, 0xe6, 0x86}};

// IID_IDXGIKeyedMutex: {9d8e1289-d7b3-465f-8126-250e349af85d}
static const IID IID_IDXGIKeyedMutex_local = {
    0x9d8e1289, 0xd7b3, 0x465f,
    {0x81, 0x26, 0x25, 0x0e, 0x34, 0x9a, 0xf8, 0x5d}};


/*
 * WGL_NV_DX_interop2 function types (loaded dynamically via wglGetProcAddress).
 * Same typedefs as in comp_gl_compositor.c:72-80.
 */
typedef HANDLE(WINAPI *PFN_wglDXOpenDeviceNV)(void *dxDevice);
typedef BOOL(WINAPI *PFN_wglDXCloseDeviceNV)(HANDLE hDevice);
typedef HANDLE(WINAPI *PFN_wglDXRegisterObjectNV)(HANDLE hDevice, void *dxObject,
                                                   GLuint name, GLenum type, GLenum access);
typedef BOOL(WINAPI *PFN_wglDXUnregisterObjectNV)(HANDLE hDevice, HANDLE hObject);
typedef BOOL(WINAPI *PFN_wglDXLockObjectsNV)(HANDLE hDevice, GLint count, HANDLE *hObjects);
typedef BOOL(WINAPI *PFN_wglDXUnlockObjectsNV)(HANDLE hDevice, GLint count, HANDLE *hObjects);

#define WGL_ACCESS_READ_WRITE_NV 0x0001

/* Cached WGL function pointers (loaded once). */
static PFN_wglDXOpenDeviceNV pfn_wglDXOpenDeviceNV;
static PFN_wglDXCloseDeviceNV pfn_wglDXCloseDeviceNV;
static PFN_wglDXRegisterObjectNV pfn_wglDXRegisterObjectNV;
static PFN_wglDXUnregisterObjectNV pfn_wglDXUnregisterObjectNV;
static PFN_wglDXLockObjectsNV pfn_wglDXLockObjectsNV;
static PFN_wglDXUnlockObjectsNV pfn_wglDXUnlockObjectsNV;
static bool wgl_funcs_loaded = false;

static bool
load_wgl_dx_interop_functions(void)
{
	if (wgl_funcs_loaded) {
		return pfn_wglDXOpenDeviceNV != NULL;
	}
	wgl_funcs_loaded = true;

	pfn_wglDXOpenDeviceNV = (PFN_wglDXOpenDeviceNV)wglGetProcAddress("wglDXOpenDeviceNV");
	pfn_wglDXCloseDeviceNV = (PFN_wglDXCloseDeviceNV)wglGetProcAddress("wglDXCloseDeviceNV");
	pfn_wglDXRegisterObjectNV = (PFN_wglDXRegisterObjectNV)wglGetProcAddress("wglDXRegisterObjectNV");
	pfn_wglDXUnregisterObjectNV = (PFN_wglDXUnregisterObjectNV)wglGetProcAddress("wglDXUnregisterObjectNV");
	pfn_wglDXLockObjectsNV = (PFN_wglDXLockObjectsNV)wglGetProcAddress("wglDXLockObjectsNV");
	pfn_wglDXUnlockObjectsNV = (PFN_wglDXUnlockObjectsNV)wglGetProcAddress("wglDXUnlockObjectsNV");

	if (!pfn_wglDXOpenDeviceNV || !pfn_wglDXCloseDeviceNV ||
	    !pfn_wglDXRegisterObjectNV || !pfn_wglDXUnregisterObjectNV ||
	    !pfn_wglDXLockObjectsNV || !pfn_wglDXUnlockObjectsNV) {
		U_LOG_E("WGL_NV_DX_interop2 not available");
		return false;
	}
	return true;
}

bool
client_gl_d3d11_interop_available(void)
{
	return load_wgl_dx_interop_functions();
}


/*
 * Down-cast helper.
 */
static inline struct client_gl_d3d11_swapchain *
client_gl_d3d11_swapchain(struct xrt_swapchain *xsc)
{
	return (struct client_gl_d3d11_swapchain *)xsc;
}


/*
 * Swapchain functions.
 */

static void
client_gl_d3d11_swapchain_destroy(struct xrt_swapchain *xsc)
{
	struct client_gl_d3d11_swapchain *sc = client_gl_d3d11_swapchain(xsc);
	struct client_gl_compositor *c = sc->base.gl_compositor;

	enum xrt_result xret = client_gl_compositor_context_begin(&c->base.base, CLIENT_GL_CONTEXT_REASON_OTHER);

	for (uint32_t i = 0; i < sc->image_count; i++) {
		if (sc->dx_interop_objects[i]) {
			pfn_wglDXUnregisterObjectNV((HANDLE)sc->dx_interop_device,
			                            (HANDLE)sc->dx_interop_objects[i]);
			sc->dx_interop_objects[i] = NULL;
		}
		if (sc->dx_keyed_mutexes[i]) {
			((IDXGIKeyedMutex *)sc->dx_keyed_mutexes[i])->lpVtbl->Release(
			    (IDXGIKeyedMutex *)sc->dx_keyed_mutexes[i]);
			sc->dx_keyed_mutexes[i] = NULL;
		}
		if (sc->dx_staging_textures[i]) {
			((ID3D11Texture2D *)sc->dx_staging_textures[i])->lpVtbl->Release(
			    (ID3D11Texture2D *)sc->dx_staging_textures[i]);
			sc->dx_staging_textures[i] = NULL;
		}
		if (sc->dx_textures[i]) {
			((ID3D11Texture2D *)sc->dx_textures[i])->lpVtbl->Release(
			    (ID3D11Texture2D *)sc->dx_textures[i]);
			sc->dx_textures[i] = NULL;
		}
	}

	if (sc->base.base.base.image_count > 0 && xret == XRT_SUCCESS) {
		glDeleteTextures(sc->base.base.base.image_count, &sc->base.base.images[0]);
	}

	if (sc->dx_interop_device) {
		pfn_wglDXCloseDeviceNV((HANDLE)sc->dx_interop_device);
		sc->dx_interop_device = NULL;
	}

	if (sc->dx_device1) {
		((ID3D11Device1 *)sc->dx_device1)->lpVtbl->Release(
		    (ID3D11Device1 *)sc->dx_device1);
		sc->dx_device1 = NULL;
	}
	if (sc->dx_context) {
		((ID3D11DeviceContext *)sc->dx_context)->lpVtbl->Release(
		    (ID3D11DeviceContext *)sc->dx_context);
		sc->dx_context = NULL;
	}
	if (sc->dx_device) {
		((ID3D11Device *)sc->dx_device)->lpVtbl->Release(
		    (ID3D11Device *)sc->dx_device);
		sc->dx_device = NULL;
	}

	if (xret == XRT_SUCCESS) {
		client_gl_compositor_context_end(&c->base.base, CLIENT_GL_CONTEXT_REASON_OTHER);
	}

	// Drop our reference, does NULL checking.
	xrt_swapchain_reference((struct xrt_swapchain **)&sc->base.xscn, NULL);

	free(sc);
}

static xrt_result_t
client_gl_d3d11_swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	struct client_gl_d3d11_swapchain *sc = client_gl_d3d11_swapchain(xsc);

	// Do the native acquire via IPC
	uint32_t index = 0;
	xrt_result_t xret = xrt_swapchain_acquire_image(&sc->base.xscn->base, &index);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	// GL texture is already locked from creation or previous release cycle.
	// No need to re-lock here.

	*out_index = index;
	return XRT_SUCCESS;
}

static xrt_result_t
client_gl_d3d11_swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	struct client_gl_d3d11_swapchain *sc = client_gl_d3d11_swapchain(xsc);

	// Ensure GL commands are flushed before unlocking
	glFlush();

	// Unlock the WGL interop so D3D11 can access the staging texture
	if (sc->dx_interop_objects[index]) {
		HANDLE obj = (HANDLE)sc->dx_interop_objects[index];
		if (!pfn_wglDXUnlockObjectsNV((HANDLE)sc->dx_interop_device, 1, &obj)) {
			U_LOG_E("wglDXUnlockObjectsNV failed for image %u: %lu", index, GetLastError());
		}
	}

	// Copy staging texture → shared texture (KeyedMutex-protected).
	// KeyedMutex serves as both a lock and cross-device memory barrier —
	// without it, CopyResource on our device isn't visible to the service's device.
	if (sc->dx_staging_textures[index] && sc->dx_textures[index]) {
		ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)sc->dx_context;

		if (sc->dx_keyed_mutexes[index]) {
			IDXGIKeyedMutex *mutex = (IDXGIKeyedMutex *)sc->dx_keyed_mutexes[index];
			mutex->lpVtbl->AcquireSync(mutex, 0, 100);
		}

		ctx->lpVtbl->CopyResource(
		    ctx,
		    (ID3D11Resource *)sc->dx_textures[index],
		    (ID3D11Resource *)sc->dx_staging_textures[index]);

		if (sc->dx_keyed_mutexes[index]) {
			IDXGIKeyedMutex *mutex = (IDXGIKeyedMutex *)sc->dx_keyed_mutexes[index];
			mutex->lpVtbl->ReleaseSync(mutex, 0);
		}
	}

	// Re-lock the interop object so GL can render to the staging texture next frame
	if (sc->dx_interop_objects[index]) {
		HANDLE obj = (HANDLE)sc->dx_interop_objects[index];
		if (!pfn_wglDXLockObjectsNV((HANDLE)sc->dx_interop_device, 1, &obj)) {
			U_LOG_E("wglDXLockObjectsNV (re-lock) failed for image %u: %lu", index, GetLastError());
		}
	}

	// Then do the native release via IPC
	return xrt_swapchain_release_image(&sc->base.xscn->base, index);
}


/*
 * Swapchain create.
 */

struct xrt_swapchain *
client_gl_d3d11_swapchain_create(struct xrt_compositor *xc,
                                 const struct xrt_swapchain_create_info *info,
                                 struct xrt_swapchain_native *xscn,
                                 struct client_gl_swapchain **out_cglsc)
{
	if (xscn == NULL) {
		return NULL;
	}

	if (!load_wgl_dx_interop_functions()) {
		U_LOG_E("WGL_NV_DX_interop2 required but not available");
		return NULL;
	}

	struct xrt_swapchain *native_xsc = &xscn->base;
	uint32_t image_count = native_xsc->image_count;

	GLuint binding_enum = 0;
	GLuint tex_target = 0;
	ogl_texture_target_for_swapchain_info(info, &tex_target, &binding_enum);

	struct client_gl_d3d11_swapchain *sc = U_TYPED_CALLOC(struct client_gl_d3d11_swapchain);
	sc->base.base.base.destroy = client_gl_d3d11_swapchain_destroy;
	sc->base.base.base.acquire_image = client_gl_d3d11_swapchain_acquire_image;
	sc->base.base.base.release_image = client_gl_d3d11_swapchain_release_image;
	sc->base.base.base.reference.count = 1;
	sc->base.base.base.image_count = image_count;
	sc->base.xscn = xscn;
	sc->base.tex_target = tex_target;
	sc->base.gl_compositor = client_gl_compositor(xc);
	sc->image_count = image_count;

	// Create D3D11 device for interop
	ID3D11Device *dx_device = NULL;
	ID3D11DeviceContext *dx_context = NULL;
	HRESULT hr = D3D11CreateDevice(
	    NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
	    NULL, 0, D3D11_SDK_VERSION,
	    &dx_device, NULL, &dx_context);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create D3D11 device for GL interop: 0x%08lx", hr);
		free(sc);
		return NULL;
	}
	sc->dx_device = dx_device;
	sc->dx_context = dx_context;

	// Query ID3D11Device1 for OpenSharedResource1 (NT handle support)
	ID3D11Device1 *dx_device1 = NULL;
	hr = dx_device->lpVtbl->QueryInterface(dx_device, &IID_ID3D11Device1_local, (void **)&dx_device1);
	if (FAILED(hr)) {
		U_LOG_E("Failed to get ID3D11Device1: 0x%08lx", hr);
		goto err_cleanup;
	}
	sc->dx_device1 = dx_device1;

	// Register D3D11 device with WGL
	HANDLE interop_device = pfn_wglDXOpenDeviceNV(dx_device);
	if (!interop_device) {
		U_LOG_E("wglDXOpenDeviceNV failed: %lu", GetLastError());
		goto err_cleanup;
	}
	sc->dx_interop_device = interop_device;

	// Generate GL texture names
	struct xrt_swapchain_gl *xscgl = &sc->base.base;
	glGenTextures(image_count, xscgl->images);

	// Import each shared texture
	for (uint32_t i = 0; i < image_count; i++) {
		HANDLE nt_handle = (HANDLE)xscn->images[i].handle;
		if (nt_handle == NULL || nt_handle == INVALID_HANDLE_VALUE) {
			U_LOG_E("Invalid NT handle for image %u", i);
			goto err_cleanup;
		}

		// Open the shared D3D11 texture via NT handle (has KeyedMutex)
		ID3D11Texture2D *dx_tex = NULL;
		hr = dx_device1->lpVtbl->OpenSharedResource1(
		    dx_device1, nt_handle,
		    &IID_ID3D11Texture2D_local, (void **)&dx_tex);
		if (FAILED(hr)) {
			U_LOG_E("OpenSharedResource1 failed for image %u: 0x%08lx", i, hr);
			goto err_cleanup;
		}
		sc->dx_textures[i] = dx_tex;

		// Get KeyedMutex interface for synchronization
		IDXGIKeyedMutex *keyed_mutex = NULL;
		hr = dx_tex->lpVtbl->QueryInterface(
		    dx_tex, &IID_IDXGIKeyedMutex_local, (void **)&keyed_mutex);
		if (SUCCEEDED(hr)) {
			sc->dx_keyed_mutexes[i] = keyed_mutex;
		}

		// We have consumed this handle, mark it invalid so IPC doesn't close it again
		xscn->images[i].handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;

		// Get shared texture descriptor for creating staging copy
		D3D11_TEXTURE2D_DESC desc;
		dx_tex->lpVtbl->GetDesc(dx_tex, &desc);

		// Create a plain staging texture (no KeyedMutex) for WGL interop.
		// WGL_NV_DX_interop doesn't support KeyedMutex textures directly.
		D3D11_TEXTURE2D_DESC staging_desc = desc;
		staging_desc.MiscFlags = 0; // No shared flags — local to this device
		staging_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

		ID3D11Texture2D *staging_tex = NULL;
		hr = dx_device->lpVtbl->CreateTexture2D(dx_device, &staging_desc, NULL, &staging_tex);
		if (FAILED(hr)) {
			U_LOG_E("Failed to create staging texture for image %u: 0x%08lx", i, hr);
			goto err_cleanup;
		}
		sc->dx_staging_textures[i] = staging_tex;

		// Register the STAGING texture (not the shared one) with WGL interop
		HANDLE interop_obj = pfn_wglDXRegisterObjectNV(
		    interop_device, staging_tex,
		    xscgl->images[i], GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
		if (!interop_obj) {
			U_LOG_E("wglDXRegisterObjectNV failed for image %u: %lu", i, GetLastError());
			goto err_cleanup;
		}
		sc->dx_interop_objects[i] = interop_obj;
	}

	// Lock all interop objects immediately so GL textures are valid for FBO setup.
	// Apps call glFramebufferTexture2D during init (before acquire/release cycle).
	for (uint32_t i = 0; i < image_count; i++) {
		if (sc->dx_interop_objects[i]) {
			HANDLE obj = (HANDLE)sc->dx_interop_objects[i];
			if (!pfn_wglDXLockObjectsNV(interop_device, 1, &obj)) {
				U_LOG_E("Initial wglDXLockObjectsNV failed for image %u: %lu", i, GetLastError());
			}
		}
	}

	U_LOG_W("GL/D3D11 interop swapchain created: %u images, %ux%u via WGL_NV_DX_interop2",
	        image_count, info->width, info->height);

	*out_cglsc = &sc->base;
	return &sc->base.base.base;

err_cleanup:
	// Clean up on failure — call destroy which handles partial init
	client_gl_d3d11_swapchain_destroy(&sc->base.base.base);
	return NULL;
}
