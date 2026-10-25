// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The OpenGL deposit (#2317 P3). See comp_gl_deposit.h.
 * @ingroup comp_gl
 */

#include "comp_gl_deposit.h"

#include "hub_d3d11/comp_hub_d3d11.h"
#include "hub_d3d11/comp_hub_d3d11_handoff.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include "ogl/ogl_api.h"
#include "ogl/wgl_api.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>

#include <stdlib.h>
#include <string.h>

/*
 * GL_EXT_semaphore_win32 is not in the generated GLAD (only GL_EXT_semaphore
 * and GL_EXT_memory_object_win32 are), so its one entry point and two tokens are
 * spelled here. Values from the GL_EXT_external_objects_win32 specification.
 */
#ifndef GL_HANDLE_TYPE_D3D12_FENCE_EXT
#define GL_HANDLE_TYPE_D3D12_FENCE_EXT 0x9594
#endif
#ifndef GL_D3D12_FENCE_VALUE_EXT
#define GL_D3D12_FENCE_VALUE_EXT 0x9595
#endif
typedef void(APIENTRY *PFN_dxr_glImportSemaphoreWin32HandleEXT)(GLuint semaphore, GLenum handleType, void *handle);

// WGL_NV_DX_interop2 (loaded through wglGetProcAddress, like the DComp path).
typedef HANDLE(WINAPI *PFN_dxr_wglDXOpenDeviceNV)(void *dxDevice);
typedef BOOL(WINAPI *PFN_dxr_wglDXCloseDeviceNV)(HANDLE hDevice);
typedef HANDLE(WINAPI *PFN_dxr_wglDXRegisterObjectNV)(
    HANDLE hDevice, void *dxObject, GLuint name, GLenum type, GLenum access);
typedef BOOL(WINAPI *PFN_dxr_wglDXUnregisterObjectNV)(HANDLE hDevice, HANDLE hObject);
typedef BOOL(WINAPI *PFN_dxr_wglDXLockObjectsNV)(HANDLE hDevice, GLint count, HANDLE *hObjects);
typedef BOOL(WINAPI *PFN_dxr_wglDXUnlockObjectsNV)(HANDLE hDevice, GLint count, HANDLE *hObjects);
#ifndef WGL_ACCESS_READ_WRITE_NV
#define WGL_ACCESS_READ_WRITE_NV 0x0001
#endif

struct comp_gl_deposit
{
	IDXGIAdapter *adapter;
	ID3D11Device *dx;
	ID3D11DeviceContext *ctx;
	ID3D11DeviceContext4 *ctx4;
	ID3D11Texture2D *tex;
	HANDLE tex_share;
	uint32_t width, height;

	//! Producer fence: "GL's atlas write is complete at fence_value".
	ID3D11Fence *fence;
	HANDLE fence_share;
	uint64_t fence_value;

	const char *import_name;
	const char *import_reason;

	// --- memory_object path ---
	bool mo;
	GLuint gl_tex;
	GLuint gl_mem;
	GLuint gl_sem_prod; //!< imports @ref fence (GL signals it)
	GLuint gl_sem_rel;  //!< imports @ref rel_fence (GL waits on it)
	//! Release fence: "the hub's back-fence for this frame is satisfied".
	//! Signalled ONLY by @ref ctx, waited ONLY by GL.
	ID3D11Fence *rel_fence;
	HANDLE rel_share;
	uint64_t rel_value;

	// --- nv_dx_interop path ---
	//! The NON-shared transit texture GL renders into (WGL_NV_DX_interop2 is
	//! only reliable on a texture the interop device owns outright); the
	//! deposit context copies it into @ref tex after every unlock.
	ID3D11Texture2D *iop_tex;
	HANDLE iop_dev;
	HANDLE iop_obj;
	bool locked;
	PFN_dxr_wglDXCloseDeviceNV CloseDeviceNV;
	PFN_dxr_wglDXUnregisterObjectNV UnregisterObjectNV;
	PFN_dxr_wglDXLockObjectsNV LockObjectsNV;
	PFN_dxr_wglDXUnlockObjectsNV UnlockObjectsNV;
};

static bool
gl_dep_has_gl_extension(const char *name)
{
	if (glGetStringi == NULL) {
		return false;
	}
	GLint n = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &n);
	for (GLint i = 0; i < n; i++) {
		const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
		if (e != NULL && strcmp(e, name) == 0) {
			return true;
		}
	}
	return false;
}

//! Drain GL's error queue (bounded) so the next check reads only our own call.
static void
gl_dep_drain_errors(void)
{
	for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++) {
	}
}

//! A shared fence on the deposit device, plus its NT handle.
static bool
gl_dep_make_fence(struct comp_gl_deposit *dep, ID3D11Fence **out_fence, HANDLE *out_share)
{
	ID3D11Device5 *d5 = nullptr;
	HRESULT hr = dep->dx->QueryInterface(__uuidof(ID3D11Device5), (void **)&d5);
	if (SUCCEEDED(hr) && d5 != nullptr) {
		hr = d5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **)out_fence);
		d5->Release();
	}
	if (SUCCEEDED(hr) && *out_fence != nullptr) {
		hr = (*out_fence)->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, out_share);
	}
	if (FAILED(hr) || *out_fence == nullptr || *out_share == nullptr) {
		U_LOG_W("GL deposit: shared fence create failed: 0x%08lx", (unsigned long)hr);
		return false;
	}
	return true;
}

//! Free the memory_object GL side only (the GL context is current).
static void
gl_dep_release_mo(struct comp_gl_deposit *dep)
{
	if (dep->gl_sem_prod != 0) {
		glDeleteSemaphoresEXT(1, &dep->gl_sem_prod);
		dep->gl_sem_prod = 0;
	}
	if (dep->gl_sem_rel != 0) {
		glDeleteSemaphoresEXT(1, &dep->gl_sem_rel);
		dep->gl_sem_rel = 0;
	}
	if (dep->gl_tex != 0 && dep->iop_obj == NULL) {
		glDeleteTextures(1, &dep->gl_tex);
		dep->gl_tex = 0;
	}
	if (dep->gl_mem != 0) {
		glDeleteMemoryObjectsEXT(1, &dep->gl_mem);
		dep->gl_mem = 0;
	}
	dep->mo = false;
}

//! The texture is GL_FRAMEBUFFER_COMPLETE as a colour attachment.
static bool
gl_dep_fbo_complete(GLuint tex)
{
	GLint prev = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	const GLenum st = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev);
	glDeleteFramebuffers(1, &fbo);
	return st == GL_FRAMEBUFFER_COMPLETE;
}

/*!
 * memory_object import: the texture as GL storage, both fences as semaphores.
 * False leaves nothing GL-side allocated.
 */
static bool
gl_dep_import_memory_object(struct comp_gl_deposit *dep, PFN_dxr_glImportSemaphoreWin32HandleEXT import_sem)
{
	gl_dep_drain_errors();

	glCreateMemoryObjectsEXT(1, &dep->gl_mem);
	// GL_HANDLE_TYPE_D3D11_IMAGE_EXT requires a dedicated memory object.
	const GLint dedicated = GL_TRUE;
	glMemoryObjectParameterivEXT(dep->gl_mem, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
	/*
	 * The import size: the driver owns the D3D11 allocation's real size and a
	 * dedicated D3D11-image import binds the whole resource. D3D11 does not
	 * report that size, so this is a CONSERVATIVE OVERSIZE: the extent padded
	 * to 128 texels each way (any tiled layout's footprint) at 4 B/texel,
	 * rounded up to the 64 KiB allocation granularity. A driver that disagrees
	 * errors here or at the storage call below (the session then takes the
	 * interop path, import_reason=memory_import_failed); one that accepts a
	 * wrong size is caught by the create-time round-trip probe.
	 */
	const GLuint64 pad_w = ((GLuint64)dep->width + 127u) & ~(GLuint64)127u;
	const GLuint64 pad_h = ((GLuint64)dep->height + 127u) & ~(GLuint64)127u;
	const GLuint64 bytes = ((pad_w * pad_h * 4u) + 0xFFFFu) & ~(GLuint64)0xFFFFu;
	U_LOG_W("GL deposit: memory_object import of %ux%u with size %llu bytes", dep->width, dep->height,
	        (unsigned long long)bytes);
	// Importing an NT handle does not transfer its ownership (the spec): the
	// deposit closes tex_share itself at destroy.
	glImportMemoryWin32HandleEXT(dep->gl_mem, bytes, GL_HANDLE_TYPE_D3D11_IMAGE_EXT, dep->tex_share);
	GLenum err = glGetError();
	if (err == GL_NO_ERROR) {
		glGenTextures(1, &dep->gl_tex);
		glBindTexture(GL_TEXTURE_2D, dep->gl_tex);
		glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)dep->width, (GLsizei)dep->height, dep->gl_mem,
		                     0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glBindTexture(GL_TEXTURE_2D, 0);
		err = glGetError();
	}
	if (err == GL_NO_ERROR) {
		glGenSemaphoresEXT(1, &dep->gl_sem_prod);
		import_sem(dep->gl_sem_prod, GL_HANDLE_TYPE_D3D12_FENCE_EXT, dep->fence_share);
		glGenSemaphoresEXT(1, &dep->gl_sem_rel);
		import_sem(dep->gl_sem_rel, GL_HANDLE_TYPE_D3D12_FENCE_EXT, dep->rel_share);
		err = glGetError();
	}
	if (err != GL_NO_ERROR || !gl_dep_fbo_complete(dep->gl_tex)) {
		U_LOG_W("GL deposit: memory_object import failed (GL error 0x%04x) — trying WGL_NV_DX_interop2",
		        (unsigned)err);
		gl_dep_release_mo(dep);
		return false;
	}
	dep->mo = true;
	return true;
}

//! WGL_NV_DX_interop2 import: register the texture on the deposit device.
static bool
gl_dep_import_interop(struct comp_gl_deposit *dep)
{
	PFN_dxr_wglDXOpenDeviceNV open_dev = (PFN_dxr_wglDXOpenDeviceNV)wglGetProcAddress("wglDXOpenDeviceNV");
	PFN_dxr_wglDXRegisterObjectNV reg = (PFN_dxr_wglDXRegisterObjectNV)wglGetProcAddress("wglDXRegisterObjectNV");
	dep->CloseDeviceNV = (PFN_dxr_wglDXCloseDeviceNV)wglGetProcAddress("wglDXCloseDeviceNV");
	dep->UnregisterObjectNV = (PFN_dxr_wglDXUnregisterObjectNV)wglGetProcAddress("wglDXUnregisterObjectNV");
	dep->LockObjectsNV = (PFN_dxr_wglDXLockObjectsNV)wglGetProcAddress("wglDXLockObjectsNV");
	dep->UnlockObjectsNV = (PFN_dxr_wglDXUnlockObjectsNV)wglGetProcAddress("wglDXUnlockObjectsNV");
	if (open_dev == NULL || reg == NULL || dep->CloseDeviceNV == NULL || dep->UnregisterObjectNV == NULL ||
	    dep->LockObjectsNV == NULL || dep->UnlockObjectsNV == NULL) {
		return false;
	}

	dep->iop_dev = open_dev(dep->dx);
	if (dep->iop_dev == NULL) {
		U_LOG_W("GL deposit: wglDXOpenDeviceNV failed (%lu)", GetLastError());
		return false;
	}
	{
		D3D11_TEXTURE2D_DESC td = {};
		dep->tex->GetDesc(&td);
		td.MiscFlags = 0; // NOT shared: the one interop shape this stack writes back reliably
		HRESULT hr = dep->dx->CreateTexture2D(&td, nullptr, &dep->iop_tex);
		if (FAILED(hr) || dep->iop_tex == nullptr) {
			U_LOG_W("GL deposit: interop transit texture create failed: 0x%08lx", (unsigned long)hr);
			dep->iop_tex = nullptr;
			dep->CloseDeviceNV(dep->iop_dev);
			dep->iop_dev = NULL;
			return false;
		}
	}
	glGenTextures(1, &dep->gl_tex);
	dep->iop_obj = reg(dep->iop_dev, dep->iop_tex, dep->gl_tex, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
	if (dep->iop_obj == NULL) {
		U_LOG_W("GL deposit: wglDXRegisterObjectNV failed (%lu)", GetLastError());
		glDeleteTextures(1, &dep->gl_tex);
		dep->gl_tex = 0;
		dep->CloseDeviceNV(dep->iop_dev);
		dep->iop_dev = NULL;
		return false;
	}
	dep->LockObjectsNV(dep->iop_dev, 1, &dep->iop_obj);
	const bool complete = gl_dep_fbo_complete(dep->gl_tex);
	dep->UnlockObjectsNV(dep->iop_dev, 1, &dep->iop_obj);
	if (!complete) {
		U_LOG_W("GL deposit: the interop texture is not framebuffer-complete");
		dep->UnregisterObjectNV(dep->iop_dev, dep->iop_obj);
		dep->iop_obj = NULL;
		glDeleteTextures(1, &dep->gl_tex);
		dep->gl_tex = 0;
		dep->CloseDeviceNV(dep->iop_dev);
		dep->iop_dev = NULL;
		return false;
	}
	return true;
}

extern "C" const char *
comp_gl_deposit_sync_name(struct comp_gl_deposit *dep)
{
	return dep != nullptr && dep->mo ? "fence" : "lock_copy";
}

/*!
 * The create-time ROUND-TRIP PROBE (the VK deposit's, for GL): GL clears the
 * atlas to a sentinel through the frame path's own publish (begin/end), the
 * deposit context waits on the producer fence and copies texel (0,0) to a
 * staging texture, and the CPU reads it back. A mismatch means the import
 * "worked" but the bytes never reach the D3D11 resource — the failure mode
 * WGL_NV_DX_interop2 write-back has on a shared texture, and the one a wrong
 * memory-object size could cause — so the session refuses
 * (`gl_deposit_unverified`) instead of weaving black. A one-time, BOUNDED CPU
 * wait (1 s) at create; never on the frame path.
 */
static bool
gl_dep_probe(struct comp_gl_deposit *dep)
{
	static const uint8_t want[4] = {0x33, 0x66, 0x99, 0xCC};

	comp_gl_deposit_begin(dep, nullptr);
	GLint prev = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
	const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_SCISSOR_TEST);
	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dep->gl_tex, 0);
	glClearColor(want[0] / 255.0f, want[1] / 255.0f, want[2] / 255.0f, want[3] / 255.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev);
	glDeleteFramebuffers(1, &fbo);
	if (scissor) {
		glEnable(GL_SCISSOR_TEST);
	}
	struct comp_hub_d3d11_handoff h = {};
	comp_gl_deposit_end(dep, /*hub_direct=*/true, dep->width, dep->height, &h);

	D3D11_TEXTURE2D_DESC sd = {};
	sd.Width = 1;
	sd.Height = 1;
	sd.MipLevels = 1;
	sd.ArraySize = 1;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D *staging = nullptr;
	if (FAILED(dep->dx->CreateTexture2D(&sd, nullptr, &staging)) || staging == nullptr) {
		U_LOG_W("GL deposit probe: staging texture create failed");
		return false;
	}
	dep->ctx4->Wait(dep->fence, dep->fence_value);
	const D3D11_BOX box = {0, 0, 0, 1, 1, 1};
	dep->ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, dep->tex, 0, &box);
	dep->ctx->Flush();

	D3D11_MAPPED_SUBRESOURCE m = {};
	HRESULT hr = DXGI_ERROR_WAS_STILL_DRAWING;
	for (int i = 0; i < 1000 && hr == DXGI_ERROR_WAS_STILL_DRAWING; i++) {
		hr = dep->ctx->Map(staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
			Sleep(1);
		}
	}
	bool ok = false;
	uint8_t got[4] = {0, 0, 0, 0};
	if (SUCCEEDED(hr)) {
		memcpy(got, m.pData, 4);
		dep->ctx->Unmap(staging, 0);
		ok = true;
		for (int k = 0; k < 4; k++) {
			const int d = (int)got[k] - (int)want[k];
			ok = ok && d >= -1 && d <= 1;
		}
	}
	staging->Release();
	if (!ok) {
		U_LOG_W(
		    "GL deposit probe: FAILED (import=%s, hr=0x%08lx, read %02x%02x%02x%02x, want %02x%02x%02x%02x) — "
		    "GL's writes do not reach the D3D11 texture; refusing (gl_deposit_unverified)",
		    dep->import_name, (unsigned long)hr, got[0], got[1], got[2], got[3], want[0], want[1], want[2],
		    want[3]);
	}
	return ok;
}

extern "C" const char *
comp_gl_deposit_create(const struct comp_gl_deposit_info *info, struct comp_gl_deposit **out_dep)
{
	*out_dep = nullptr;
	if (info == nullptr || info->dxgi_adapter == nullptr || info->width == 0 || info->height == 0) {
		return COMP_HUB_REASON_GL_NO_LUID;
	}

	/*
	 * Which paths this driver offers, before anything is created. memory_object
	 * needs the texture import, the semaphore import (both GL_EXT_*_win32) and
	 * an EXACT adapter identity; interop needs only the WGL entry points.
	 */
	const bool have_mo = GLAD_GL_EXT_memory_object && GLAD_GL_EXT_memory_object_win32 && GLAD_GL_EXT_semaphore &&
	                     glImportMemoryWin32HandleEXT != NULL && glTexStorageMem2DEXT != NULL;
	PFN_dxr_glImportSemaphoreWin32HandleEXT import_sem = nullptr;
	if (have_mo && gl_dep_has_gl_extension("GL_EXT_semaphore_win32")) {
		import_sem =
		    (PFN_dxr_glImportSemaphoreWin32HandleEXT)wglGetProcAddress("glImportSemaphoreWin32HandleEXT");
	}
	const bool have_iop = wglGetProcAddress("wglDXOpenDeviceNV") != NULL;

	// DXR_HUB_GL_IMPORT=interop forces the fallback (A/B control);
	// =memory_object forbids it.
	const char *force = getenv("DXR_HUB_GL_IMPORT");
	const bool force_iop = force != nullptr && strcmp(force, "interop") == 0;
	const bool forbid_iop = force != nullptr && strcmp(force, "memory_object") == 0;

	const char *mo_unusable = nullptr;
	if (force_iop) {
		mo_unusable = COMP_GL_DEPOSIT_IMPORT_REASON_FORCED;
	} else if (!have_mo) {
		mo_unusable = COMP_GL_DEPOSIT_IMPORT_REASON_NO_MEMORY_OBJECT;
	} else if (import_sem == nullptr) {
		mo_unusable = COMP_GL_DEPOSIT_IMPORT_REASON_NO_SEMAPHORE;
	} else if (!info->luid_exact) {
		mo_unusable = COMP_GL_DEPOSIT_IMPORT_REASON_LUID_HEURISTIC;
	}
	if (mo_unusable != nullptr && (!have_iop || forbid_iop)) {
		U_LOG_W("GL deposit: no import path — memory_object unusable (%s), WGL_NV_DX_interop2 %s", mo_unusable,
		        forbid_iop ? "forbidden by DXR_HUB_GL_IMPORT=memory_object" : "absent");
		return COMP_HUB_REASON_GL_NO_INTEROP;
	}

	struct comp_gl_deposit *dep = U_TYPED_CALLOC(struct comp_gl_deposit);
	if (dep == nullptr) {
		return COMP_HUB_REASON_GL_DEPOSIT_FAILED;
	}
	dep->width = info->width;
	dep->height = info->height;
	dep->adapter = (IDXGIAdapter *)info->dxgi_adapter;
	dep->adapter->AddRef();

	// The deposit device, ON the GL context's adapter (ADR-037 §5, #1159).
	HRESULT hr = D3D11CreateDevice(dep->adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
	                               nullptr, 0, D3D11_SDK_VERSION, &dep->dx, nullptr, &dep->ctx);
	if (SUCCEEDED(hr) && dep->ctx != nullptr) {
		hr = dep->ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&dep->ctx4);
	}
	if (FAILED(hr) || dep->dx == nullptr || dep->ctx4 == nullptr) {
		U_LOG_W("GL deposit: D3D11 device on the GL adapter failed: 0x%08lx", (unsigned long)hr);
		comp_gl_deposit_destroy(&dep);
		return COMP_HUB_REASON_GL_DEPOSIT_FAILED;
	}

	// The atlas: encoded bytes in a typed UNORM texture (ADR-044), exactly the
	// GL_RGBA8 atlas the native leg paints.
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = dep->width;
	td.Height = dep->height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
	hr = dep->dx->CreateTexture2D(&td, nullptr, &dep->tex);
	if (SUCCEEDED(hr) && dep->tex != nullptr) {
		IDXGIResource1 *r1 = nullptr;
		hr = dep->tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1);
		if (SUCCEEDED(hr) && r1 != nullptr) {
			hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
			                            nullptr, &dep->tex_share);
			r1->Release();
		}
	}
	if (FAILED(hr) || dep->tex_share == nullptr || !gl_dep_make_fence(dep, &dep->fence, &dep->fence_share)) {
		U_LOG_W("GL deposit: shared atlas texture create failed: 0x%08lx", (unsigned long)hr);
		comp_gl_deposit_destroy(&dep);
		return COMP_HUB_REASON_GL_DEPOSIT_FAILED;
	}

	if (mo_unusable == nullptr) {
		if (gl_dep_make_fence(dep, &dep->rel_fence, &dep->rel_share) &&
		    gl_dep_import_memory_object(dep, import_sem)) {
			dep->import_name = COMP_GL_DEPOSIT_IMPORT_MEMORY_OBJECT;
		} else {
			mo_unusable = COMP_GL_DEPOSIT_IMPORT_REASON_IMPORT_FAILED;
		}
	}
	if (dep->import_name == nullptr && have_iop && !forbid_iop && gl_dep_import_interop(dep)) {
		dep->import_name = COMP_GL_DEPOSIT_IMPORT_NV_DX_INTEROP;
		dep->import_reason = mo_unusable;
	}
	if (dep->import_name == nullptr) {
		U_LOG_W("GL deposit: every import path failed (memory_object: %s)",
		        mo_unusable != nullptr ? mo_unusable : "?");
		comp_gl_deposit_destroy(&dep);
		return COMP_HUB_REASON_GL_DEPOSIT_FAILED;
	}
	if (!gl_dep_probe(dep)) {
		comp_gl_deposit_destroy(&dep);
		return COMP_HUB_REASON_GL_DEPOSIT_UNVERIFIED;
	}

	U_LOG_W(
	    "GL deposit: ACTIVE (import=%s%s%s sync=%s) — %ux%u NT-shared D3D11 atlas on the GL context's "
	    "adapter, round-trip probe OK; %s. No glFinish (#2317 P3)",
	    dep->import_name, dep->import_reason != nullptr ? " import_reason=" : "",
	    dep->import_reason != nullptr ? dep->import_reason : "", comp_gl_deposit_sync_name(dep), dep->width,
	    dep->height,
	    dep->mo ? "GL renders straight into it (zero copies); completion and release are shared ID3D11Fences "
	              "imported as GL semaphores (GPU-side)"
	            : "GL renders into a non-shared interop transit texture bracketed by wglDXLock/UnlockObjectsNV "
	              "(the lock is a CPU sync point); the deposit context copies it in after the unlock (ONE GPU "
	              "copy per frame on this fallback rung) and signals the shared fence");
	*out_dep = dep;
	return nullptr;
}

extern "C" void
comp_gl_deposit_destroy(struct comp_gl_deposit **dep_ptr)
{
	struct comp_gl_deposit *dep = *dep_ptr;
	if (dep == nullptr) {
		return;
	}
	*dep_ptr = nullptr;

	if (dep->iop_obj != NULL) {
		if (dep->locked) {
			dep->UnlockObjectsNV(dep->iop_dev, 1, &dep->iop_obj);
			dep->locked = false;
		}
		dep->UnregisterObjectNV(dep->iop_dev, dep->iop_obj);
		dep->iop_obj = NULL;
		if (dep->gl_tex != 0) {
			glDeleteTextures(1, &dep->gl_tex);
			dep->gl_tex = 0;
		}
	}
	if (dep->iop_dev != NULL) {
		dep->CloseDeviceNV(dep->iop_dev);
		dep->iop_dev = NULL;
	}
	if (dep->mo) {
		gl_dep_release_mo(dep);
	}
	if (dep->rel_share != nullptr) {
		CloseHandle(dep->rel_share);
	}
	if (dep->rel_fence != nullptr) {
		dep->rel_fence->Release();
	}
	if (dep->fence_share != nullptr) {
		CloseHandle(dep->fence_share);
	}
	if (dep->fence != nullptr) {
		dep->fence->Release();
	}
	if (dep->iop_tex != nullptr) {
		dep->iop_tex->Release();
	}
	if (dep->tex_share != nullptr) {
		CloseHandle(dep->tex_share);
	}
	if (dep->tex != nullptr) {
		dep->tex->Release();
	}
	if (dep->ctx4 != nullptr) {
		dep->ctx4->Release();
	}
	if (dep->ctx != nullptr) {
		dep->ctx->ClearState();
		dep->ctx->Flush();
		dep->ctx->Release();
	}
	if (dep->dx != nullptr) {
		dep->dx->Release();
	}
	if (dep->adapter != nullptr) {
		dep->adapter->Release();
	}
	free(dep);
}

extern "C" uint32_t
comp_gl_deposit_gl_texture(struct comp_gl_deposit *dep)
{
	return dep != nullptr ? dep->gl_tex : 0;
}

extern "C" const char *
comp_gl_deposit_import_name(struct comp_gl_deposit *dep)
{
	return dep != nullptr ? dep->import_name : "";
}

extern "C" const char *
comp_gl_deposit_import_reason(struct comp_gl_deposit *dep)
{
	return dep != nullptr ? dep->import_reason : nullptr;
}

extern "C" void
comp_gl_deposit_fill_handoff(struct comp_gl_deposit *dep, struct comp_hub_d3d11_handoff *h)
{
	memset(h, 0, sizeof(*h));
	h->d3d11_device = dep->dx;
	h->d3d11_context = dep->ctx;
	h->dxgi_adapter = dep->adapter;
	h->texture = dep->tex;
	h->shared_handle = dep->tex_share;
	h->fence = dep->fence;
	h->fence_shared_handle = dep->fence_share;
	h->fence_value = dep->fence_value;
	h->width = dep->width;
	h->height = dep->height;
	h->dxgi_format = (uint32_t)DXGI_FORMAT_R8G8B8A8_UNORM;
	// One texture for the deposit's whole life: re-opened by the hub never.
	h->generation = 1;
}

extern "C" void
comp_gl_deposit_begin(struct comp_gl_deposit *dep, struct comp_hub_d3d11 *hub)
{
	if (dep == nullptr) {
		return;
	}
	// The hub's back-fence (DIRECT: its last read; xbridge: the bridge's
	// producer copy), queued on the deposit's D3D11 context. GPU-side.
	comp_hub_d3d11_pre_render(hub);

	if (dep->mo) {
		// ...then lift it to GL: the context signals the release fence behind
		// that wait, and GL's next atlas write waits for the value.
		dep->rel_value++;
		dep->ctx4->Signal(dep->rel_fence, dep->rel_value);
		dep->ctx->Flush();
		glSemaphoreParameterui64vEXT(dep->gl_sem_rel, GL_D3D12_FENCE_VALUE_EXT, &dep->rel_value);
		const GLenum layout = GL_LAYOUT_GENERAL_EXT;
		glWaitSemaphoreEXT(dep->gl_sem_rel, 0, nullptr, 1, &dep->gl_tex, &layout);
		return;
	}

	/*
	 * Interop: the hub reads only the SHARED deposit, which only this context
	 * writes (the copy in comp_gl_deposit_end, behind the wait just queued), so
	 * that edge is in-stream. GL writes only the transit, whose last D3D11
	 * access was the previous frame's copy out of it — a command on the
	 * registered resource, which is exactly what the interop lock orders GL
	 * after.
	 */
	dep->ctx->Flush();
	if (!dep->locked) {
		dep->LockObjectsNV(dep->iop_dev, 1, &dep->iop_obj);
		dep->locked = true;
	}
}

extern "C" void
comp_gl_deposit_end(struct comp_gl_deposit *dep,
                    bool hub_direct,
                    uint32_t content_w,
                    uint32_t content_h,
                    struct comp_hub_d3d11_handoff *h)
{
	if (dep == nullptr) {
		return;
	}
	dep->fence_value++;
	if (dep->mo) {
		glSemaphoreParameterui64vEXT(dep->gl_sem_prod, GL_D3D12_FENCE_VALUE_EXT, &dep->fence_value);
		const GLenum layout = GL_LAYOUT_GENERAL_EXT;
		glSignalSemaphoreEXT(dep->gl_sem_prod, 0, nullptr, 1, &dep->gl_tex, &layout);
		// Submits the signal; never waits.
		glFlush();
		if (!hub_direct) {
			// comp_xbridge reads the atlas behind this context's stream.
			dep->ctx4->Wait(dep->fence, dep->fence_value);
		}
	} else {
		glFlush();
		if (dep->locked) {
			dep->UnlockObjectsNV(dep->iop_dev, 1, &dep->iop_obj);
			dep->locked = false;
		}
		// After the unlock the interop contract orders D3D11 commands that
		// ACCESS the registered transit behind GL's writes: this copy is one,
		// and the fence signal follows it in this context's stream.
		// Only the content box the hub crops and weaves (top-left): a full
		// copy of the worst-case atlas measured 54% app GPU vs 9.6% native on
		// an Intel UHD (#2317 P3 panel).
		const D3D11_BOX box = {0,
		                       0,
		                       0,
		                       content_w < dep->width ? content_w : dep->width,
		                       content_h < dep->height ? content_h : dep->height,
		                       1};
		dep->ctx->CopySubresourceRegion(dep->tex, 0, 0, 0, 0, dep->iop_tex, 0, &box);
		dep->ctx4->Signal(dep->fence, dep->fence_value);
		dep->ctx->Flush();
	}
	comp_gl_deposit_fill_handoff(dep, h);
}
