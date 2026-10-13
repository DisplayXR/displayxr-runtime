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
	*out_in_kinds = (1ull << XR_LIFT_IMAGE_KIND_D3D11_NT_DXR) | (1ull << XR_LIFT_IMAGE_KIND_D3D11_DXGI_DXR);
	*out_export_kinds = 1ull << XR_LIFT_IMAGE_KIND_D3D11_NT_DXR;
#else
	(void)sess;
#endif
}

#endif // OXR_HAVE_DXR_lift
