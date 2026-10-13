// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift: the graphics-API / platform half of the conversion
 *         entry points (split from oxr_lift.c, ADR-053 P3, #2054).
 * @author David Fattal
 * @ingroup oxr_api
 *
 * What depends on the session's graphics API or on Win32 lives here, so
 * oxr_conv.c stays API-neutral:
 *  - which in-process sessions reach the conversion service over a lift-only
 *    connection (ADR-049: D3D11 / D3D12 on Windows) and creating that link;
 *  - which image kinds a session can hand in and take out (the
 *    XrLiftTaskPropertiesDXR imageKinds / exportKinds this session sees).
 *
 * The service transport takes and gives D3D11 shared textures (NT or legacy
 * DXGI handles, keyed mutex) plus a shared fence; any session that can open a
 * D3D NT handle (D3D11, D3D12, and IPC sessions, whose apps are D3D on
 * Windows) can use them. Other kinds (D3D12 NT + fence, CPU, Vulkan, dma-buf,
 * IOSurface, AHardwareBuffer) arrive as capability data, with no version bump.
 */

#include "oxr_objects.h"
#include "oxr_conv_gpu.h"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_instance.h"

#include "client/ipc_client_lift_link.h"

#ifdef XRT_OS_WINDOWS
#include "xrt/xrt_windows.h"
#endif

#include <stdio.h>
#include <string.h>

#ifdef OXR_HAVE_DXR_lift

bool
oxr_conv_inprocess_eligible(struct oxr_session *sess)
{
#ifdef XRT_OS_WINDOWS
	return sess->is_d3d11_native_compositor || sess->is_d3d12_native_compositor;
#else
	(void)sess;
	return false;
#endif
}

struct ipc_client_lift_link *
oxr_conv_link_get(struct oxr_session *sess)
{
#ifdef XRT_OS_WINDOWS
	struct ipc_client_lift_link *link = (struct ipc_client_lift_link *)InterlockedCompareExchangePointer(
	    (PVOID volatile *)&sess->lift_link, NULL, NULL);
	if (link != NULL) {
		return link;
	}

	// Name the connection after the executable so `displayxr-cli clients` and
	// the service log say whose it is.
	char exe[MAX_PATH] = {0};
	const char *base = "app";
	if (GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe)) > 0) {
		const char *slash = strrchr(exe, '\\');
		base = slash != NULL ? slash + 1 : exe;
	}
	char label[XRT_MAX_APPLICATION_NAME_SIZE];
	snprintf(label, sizeof(label), "%s (in-process lift)", base);

	struct ipc_client_lift_link *made = NULL;
	if (ipc_client_lift_link_create(label, &made) != XRT_SUCCESS) {
		return NULL;
	}
	// Two threads may race the first lift call of a session: one link wins.
	link = (struct ipc_client_lift_link *)InterlockedCompareExchangePointer((PVOID volatile *)&sess->lift_link,
	                                                                        made, NULL);
	if (link != NULL) {
		ipc_client_lift_link_destroy(&made);
		return link;
	}
	return made;
#else
	(void)sess;
	return NULL;
#endif
}

void
oxr_conv_session_image_kinds(struct oxr_session *sess, uint64_t *out_in_kinds, uint64_t *out_export_kinds)
{
	*out_in_kinds = 0;
	*out_export_kinds = 0;
#ifdef XRT_OS_WINDOWS
	// D3D shared textures: IPC sessions (their apps hand in D3D11 handles on
	// Windows) and in-process D3D11 / D3D12 sessions. GL / Vulkan in-process
	// sessions see the tasks with 0 kinds until their transports land.
	const bool native = sess->is_d3d11_native_compositor || sess->is_d3d12_native_compositor ||
	                    sess->is_metal_native_compositor || sess->is_gl_native_compositor ||
	                    sess->is_vk_native_compositor;
	if (native && !oxr_conv_inprocess_eligible(sess)) {
		return;
	}
	// The service masks these by what its transport takes (conv_enum_task).
	*out_in_kinds = (1ull << XR_LIFT_IMAGE_KIND_D3D11_NT_DXR) | (1ull << XR_LIFT_IMAGE_KIND_D3D11_DXGI_DXR) |
	                (1ull << XR_LIFT_IMAGE_KIND_D3D12_NT_DXR) | (1ull << XR_LIFT_IMAGE_KIND_CPU_DXR);
	*out_export_kinds = (1ull << XR_LIFT_IMAGE_KIND_D3D11_NT_DXR) | (1ull << XR_LIFT_IMAGE_KIND_CPU_DXR);
#else
	(void)sess;
#endif
}

bool
oxr_conv_cpu_upload(const void *bytes, uint32_t pitch, uint32_t x, uint32_t y, uint32_t w, uint32_t h, void **out_handle)
{
	*out_handle = NULL;
#ifdef XRT_OS_WINDOWS
	const uint64_t size = (uint64_t)w * h * 4u;
	HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)(size >> 32),
	                              (DWORD)(size & 0xffffffffu), NULL);
	if (m == NULL) {
		return false;
	}
	uint8_t *view = (uint8_t *)MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, 0);
	if (view == NULL) {
		CloseHandle(m);
		return false;
	}
	const uint8_t *src = (const uint8_t *)bytes + (size_t)y * pitch + (size_t)x * 4u;
	for (uint32_t row = 0; row < h; row++) {
		memcpy(view + (size_t)row * w * 4u, src + (size_t)row * pitch, (size_t)w * 4u);
	}
	UnmapViewOfFile(view);
	*out_handle = m;
	return true;
#else
	(void)bytes;
	(void)pitch;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	return false;
#endif
}

void *
oxr_conv_cpu_map(void *handle, uint64_t bytes)
{
#ifdef XRT_OS_WINDOWS
	if (handle == NULL || bytes == 0) {
		return NULL;
	}
	return MapViewOfFile((HANDLE)handle, FILE_MAP_READ, 0, 0, (SIZE_T)bytes);
#else
	(void)handle;
	(void)bytes;
	return NULL;
#endif
}

void
oxr_conv_cpu_unmap(void *view)
{
#ifdef XRT_OS_WINDOWS
	if (view != NULL) {
		UnmapViewOfFile(view);
	}
#else
	(void)view;
#endif
}

void
oxr_conv_close_handle(void *handle)
{
#ifdef XRT_OS_WINDOWS
	if (handle != NULL) {
		CloseHandle((HANDLE)handle);
	}
#else
	(void)handle;
#endif
}

#endif // OXR_HAVE_DXR_lift
