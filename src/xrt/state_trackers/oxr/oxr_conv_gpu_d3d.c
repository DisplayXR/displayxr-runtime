// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift: the graphics-API / platform half of the conversion
 *         entry points (split from oxr_lift.c, ADR-053 P3, #2054).
 * @author David Fattal
 * @ingroup oxr_api
 *
 * What depends on the session's graphics API or on the OS lives here, so
 * oxr_conv.c stays API-neutral:
 *  - which in-process sessions reach the conversion service over a lift-only
 *    connection (ADR-049; every in-process native compositor on Windows,
 *    desktop Linux and macOS) and creating that link;
 *  - which spec-3 callers exist in-process (D3D11 / D3D12 on Windows: spec 3
 *    is D3D11 shared textures by definition);
 *  - which image kinds a session can hand in and take out (the
 *    XrLiftTaskPropertiesDXR imageKinds / exportKinds this session sees);
 *  - the CPU kind's shared sections (a file mapping on Windows; on POSIX the
 *    IPC transport's CPU buffer, ipc_cpu_buffer.h).
 *
 * On Windows the service transport takes and gives D3D11 shared textures (NT
 * or legacy DXGI handles, keyed mutex) plus a shared fence; any session that
 * can open a D3D NT handle (D3D11, D3D12, and IPC sessions, whose apps are D3D
 * on Windows) can use them, and every session can use CPU bytes. On POSIX the
 * service has a CPU glue only (ipc_server_conv_cpu.h): CPU in, CPU out. Other
 * kinds (Vulkan, dma-buf, IOSurface textures, AHardwareBuffer) arrive as
 * capability data, with no version bump (ADR-053 §7).
 */

#include "oxr_objects.h"
#include "oxr_conv_gpu.h"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_instance.h"

#include "client/ipc_client_service_link.h"

#ifdef XRT_OS_WINDOWS
#include "xrt/xrt_windows.h"
#else
#include "shared/ipc_cpu_buffer.h"
#include "util/u_handles.h"
#include <stdlib.h>
#endif
#ifdef XRT_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach-o/dyld.h> // _NSGetExecutablePath
#endif
#if (defined(XRT_OS_LINUX) && !defined(XRT_OS_ANDROID)) || defined(XRT_OS_MACOS)
#include <unistd.h> // readlink, getpid
#endif

#include "util/u_debug.h"

#include <stdio.h>
#include <string.h>

/*!
 * Where an in-process lift link can be opened: Windows (named pipe), desktop
 * Linux (the socket-activated service socket) and macOS (the launchd-started
 * service socket). Never launched: a plain POSIX socket connect does not
 * start a service, and on Windows the link checks the service's singleton
 * first (ipc_client_service_link.c).
 */
#if defined(XRT_OS_WINDOWS) || defined(XRT_OS_MACOS) || (defined(XRT_OS_LINUX) && !defined(XRT_OS_ANDROID))
#define OXR_CONV_HAVE_LINK 1
#endif

#ifdef OXR_HAVE_DXR_lift

//! Any in-process native compositor (an IPC session has none of these flags).
static bool
session_is_inprocess(const struct oxr_session *sess)
{
	return sess->is_d3d11_native_compositor || sess->is_d3d12_native_compositor ||
	       sess->is_metal_native_compositor || sess->is_gl_native_compositor || sess->is_vk_native_compositor;
}

bool
oxr_conv_inprocess_eligible(struct oxr_session *sess)
{
#if defined(XRT_OS_WINDOWS)
	// D3D11 / D3D12: D3D shared textures + CPU; GL / Vulkan: CPU (file mappings).
	return session_is_inprocess(sess);
#elif defined(OXR_CONV_HAVE_LINK)
	// Vulkan / GL / Metal: CPU bytes over the IPC transport's CPU buffer.
	return session_is_inprocess(sess) && ipc_cpu_buffer_supported();
#else
	(void)sess;
	return false;
#endif
}

bool
oxr_conv_v3_eligible(struct oxr_session *sess)
{
#ifdef XRT_OS_WINDOWS
	return sess->is_d3d11_native_compositor || sess->is_d3d12_native_compositor;
#else
	(void)sess;
	return false;
#endif
}

#if defined(OXR_CONV_HAVE_LINK) && !defined(XRT_OS_WINDOWS)
//! This process's executable name (the connection label), or "app".
static void
exe_basename(char *out, size_t size)
{
	char path[1024] = {0};
	bool ok = false;
#if defined(XRT_OS_MACOS)
	uint32_t len = (uint32_t)sizeof(path);
	ok = _NSGetExecutablePath(path, &len) == 0;
#else
	const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
	ok = n > 0;
	if (ok) {
		path[n] = '\0';
	}
#endif
	const char *base = "app";
	if (ok && path[0] != '\0') {
		const char *slash = strrchr(path, '/');
		base = slash != NULL ? slash + 1 : path;
	}
	(void)snprintf(out, size, "%s", base);
}

//! Pointer compare-and-swap (no <stdatomic.h>: the MinGW / MSVC rule).
static struct ipc_client_service_link *
link_cas(struct ipc_client_service_link **p,
         struct ipc_client_service_link *expected,
         struct ipc_client_service_link *desired)
{
	return __sync_val_compare_and_swap(p, expected, desired);
}
#endif

#if defined(XRT_OS_WINDOWS)
// ADR-061: the client-side read of the service's own kill switch, so an
// opted-out machine's apps never open a link just to share.
DEBUG_GET_ONCE_BOOL_OPTION(app_share_link, "DXR_APP_SHARE", true)
#endif

bool
oxr_service_link_share_wanted(struct oxr_session *sess)
{
#if defined(XRT_OS_WINDOWS)
	// Only where a tap can exist: the D3D11 fill arm (comp_xbridge_share_tap.h)
	// that the D3D11, D3D12 and Vulkan in-process tiers feed. GL has none; nor
	// has macOS / Linux in v1 — those never dial the service to share (an APP
	// slot of IPC_MAX_CLIENTS each, and a socket activation on Linux, for
	// nothing; review #2533 item 5). Even here the link dials only once the
	// compositor reported its tap state (ipc_client_service_link.c).
	return sess != NULL &&
	       (sess->is_d3d11_native_compositor || sess->is_d3d12_native_compositor ||
	        sess->is_vk_native_compositor) &&
	       debug_get_bool_option_app_share_link();
#else
	(void)sess;
	return false;
#endif
}

uint32_t
oxr_service_link_caps(struct oxr_session *sess)
{
	uint32_t caps = 0;
	if (sess != NULL && sess->sys != NULL && sess->sys->inst != NULL && sess->sys->inst->extensions.DXR_lift) {
		caps |= XRT_SHARE_LINK_CAP_LIFT;
	}
	if (oxr_service_link_share_wanted(sess)) {
		caps |= XRT_SHARE_LINK_CAP_SHARE;
	}
	return caps;
}

static uint32_t
session_tier(const struct oxr_session *sess)
{
	if (sess->is_d3d11_native_compositor) {
		return XRT_SHARE_LINK_TIER_D3D11;
	}
	if (sess->is_d3d12_native_compositor) {
		return XRT_SHARE_LINK_TIER_D3D12;
	}
	if (sess->is_vk_native_compositor) {
		return XRT_SHARE_LINK_TIER_VULKAN;
	}
	if (sess->is_gl_native_compositor) {
		return XRT_SHARE_LINK_TIER_GL;
	}
	if (sess->is_metal_native_compositor) {
		return XRT_SHARE_LINK_TIER_METAL;
	}
	return XRT_SHARE_LINK_TIER_UNKNOWN;
}

void
oxr_service_link_fill_registration(struct oxr_session *sess, bool running, struct xrt_share_link_registration *out)
{
	memset(out, 0, sizeof(*out));
	out->caps = oxr_service_link_caps(sess);
	out->opt_out = sess->app_share_opt_out ? 1u : 0u;
	out->tier = session_tier(sess);
	out->session_running = running ? 1u : 0u;
#if defined(XRT_OS_WINDOWS)
	out->pid = (uint32_t)GetCurrentProcessId();
	char exe[MAX_PATH] = {0};
	if (GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe)) > 0) {
		snprintf(out->exe, sizeof(out->exe), "%s", exe);
	}
#elif defined(OXR_CONV_HAVE_LINK)
	out->pid = (uint32_t)getpid();
	exe_basename(out->exe, sizeof(out->exe));
#endif
	if (sess->sys != NULL && sess->sys->inst != NULL && sess->sys->inst->xinst != NULL) {
		snprintf(out->app_name, sizeof(out->app_name), "%s",
		         sess->sys->inst->xinst->instance_info.app_info.application_name);
	}
}

struct ipc_client_service_link *
oxr_conv_link_get(struct oxr_session *sess)
{
#ifdef XRT_OS_WINDOWS
	struct ipc_client_service_link *link = (struct ipc_client_service_link *)InterlockedCompareExchangePointer(
	    (PVOID volatile *)&sess->service_link, NULL, NULL);
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
	snprintf(label, sizeof(label), "%s (in-process)", base);

	struct ipc_client_service_link *made = NULL;
	if (ipc_client_service_link_create(label, oxr_service_link_caps(sess), &made) != XRT_SUCCESS) {
		return NULL;
	}
	if ((ipc_client_service_link_caps(made) & XRT_SHARE_LINK_CAP_SHARE) != 0) {
		struct xrt_share_link_registration reg;
		oxr_service_link_fill_registration(sess, false, &reg); // xrBeginSession says "running"
		ipc_client_service_link_set_registration(made, &reg);  // not connected yet: stored
	}
	// Two threads may race the first use of a session's link: one link wins.
	link = (struct ipc_client_service_link *)InterlockedCompareExchangePointer(
	    (PVOID volatile *)&sess->service_link, made, NULL);
	if (link != NULL) {
		ipc_client_service_link_destroy(&made);
		return link;
	}
	return made;
#elif defined(OXR_CONV_HAVE_LINK)
	struct ipc_client_service_link *link = link_cas(&sess->service_link, NULL, NULL);
	if (link != NULL) {
		return link;
	}
	// Name the connection after the executable so `displayxr-cli clients` and
	// the service log say whose it is.
	char base[256];
	exe_basename(base, sizeof(base));
	char label[XRT_MAX_APPLICATION_NAME_SIZE];
	snprintf(label, sizeof(label), "%s (in-process)", base);

	struct ipc_client_service_link *made = NULL;
	if (ipc_client_service_link_create(label, oxr_service_link_caps(sess), &made) != XRT_SUCCESS) {
		return NULL;
	}
	if ((ipc_client_service_link_caps(made) & XRT_SHARE_LINK_CAP_SHARE) != 0) {
		struct xrt_share_link_registration reg;
		oxr_service_link_fill_registration(sess, false, &reg); // xrBeginSession says "running"
		ipc_client_service_link_set_registration(made, &reg);  // not connected yet: stored
	}
	// Two threads may race the first use of a session's link: one link wins.
	link = link_cas(&sess->service_link, NULL, made);
	if (link != NULL) {
		ipc_client_service_link_destroy(&made);
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
	oxr_conv_session_image_kinds_offer(sess, out_in_kinds, out_export_kinds);
#ifndef XRT_OS_WINDOWS
	// Step E: a GPU kind only once the service reported it takes / gives it.
	const uint64_t cpu = 1ull << XR_LIFT_IMAGE_KIND_CPU_DXR;
	*out_in_kinds &= cpu | sess->lift_service_in_kinds;
	*out_export_kinds &= cpu | sess->lift_service_export_kinds;
#endif
}

void
oxr_conv_learn_service_kinds(struct oxr_session *sess, uint64_t in_kinds, uint64_t export_kinds)
{
#ifndef XRT_OS_WINDOWS
	const uint64_t cpu = 1ull << XR_LIFT_IMAGE_KIND_CPU_DXR;
	sess->lift_service_in_kinds = in_kinds & ~cpu;
	sess->lift_service_export_kinds = export_kinds & ~cpu;
#else
	(void)sess;
	(void)in_kinds;
	(void)export_kinds;
#endif
}

void
oxr_conv_session_image_kinds_offer(struct oxr_session *sess, uint64_t *out_in_kinds, uint64_t *out_export_kinds)
{
	*out_in_kinds = 0;
	*out_export_kinds = 0;
	const uint64_t cpu = 1ull << XR_LIFT_IMAGE_KIND_CPU_DXR;
#ifdef XRT_OS_WINDOWS
	// D3D shared textures: IPC sessions (their apps hand in D3D11 handles on
	// Windows) and in-process D3D11 / D3D12 sessions. GL / Vulkan in-process
	// sessions take and give CPU bytes (file mappings).
	if (session_is_inprocess(sess) && !oxr_conv_v3_eligible(sess)) {
		if (oxr_conv_inprocess_eligible(sess)) {
			*out_in_kinds = cpu;
			*out_export_kinds = cpu;
		}
		return;
	}
	// The service masks these by what its transport takes (conv_enum_task).
	*out_in_kinds = (1ull << XR_LIFT_IMAGE_KIND_D3D11_NT_DXR) | (1ull << XR_LIFT_IMAGE_KIND_D3D11_DXGI_DXR) |
	                (1ull << XR_LIFT_IMAGE_KIND_D3D12_NT_DXR) | (1ull << XR_LIFT_IMAGE_KIND_CPU_DXR);
	*out_export_kinds = (1ull << XR_LIFT_IMAGE_KIND_D3D11_NT_DXR) | (1ull << XR_LIFT_IMAGE_KIND_CPU_DXR);
#else
	// POSIX v1 (roadmap conversion-providers.md §4.2): CPU bytes in and out,
	// for in-process sessions (the lift link) and IPC sessions alike; the
	// service masks them by what its glue takes (its CPU glue: exactly CPU).
	if (session_is_inprocess(sess) ? oxr_conv_inprocess_eligible(sess) : ipc_cpu_buffer_supported()) {
		*out_in_kinds = cpu;
		*out_export_kinds = cpu;
	}
	// ADR-053 P5 step E: an in-process Vulkan session can hand its own images
	// in and import results as the platform's GPU kind -- an IOSurface
	// (VK_EXT_metal_objects) on macOS; an OPAQUE_FD allocation or a dma-buf
	// (VK_KHR_external_memory_fd / VK_EXT_external_memory_dma_buf) on Linux.
	// Results leave as OPAQUE_FD only: XrLiftResultExportDXR cannot describe a
	// dma-buf's modifier / stride. The service masks what its glue takes.
	if (session_is_inprocess(sess) && sess->is_vk_native_compositor && oxr_conv_inprocess_eligible(sess)) {
#if defined(XRT_OS_MACOS)
		*out_in_kinds |= 1ull << XR_LIFT_IMAGE_KIND_IOSURFACE_DXR;
		*out_export_kinds |= 1ull << XR_LIFT_IMAGE_KIND_IOSURFACE_DXR;
#elif defined(XRT_OS_LINUX) && !defined(XRT_OS_ANDROID)
		*out_in_kinds |= (1ull << XR_LIFT_IMAGE_KIND_OPAQUE_FD_DXR) | (1ull << XR_LIFT_IMAGE_KIND_DMABUF_DXR);
		*out_export_kinds |= 1ull << XR_LIFT_IMAGE_KIND_OPAQUE_FD_DXR;
#endif
	}
#endif
}

bool
oxr_conv_iosurface_extent(void *iosurface, uint32_t *out_w, uint32_t *out_h)
{
	*out_w = 0;
	*out_h = 0;
#if defined(XRT_OS_MACOS)
	IOSurfaceRef s = (IOSurfaceRef)iosurface;
	if (s == NULL || CFGetTypeID(s) != IOSurfaceGetTypeID()) {
		return false;
	}
	*out_w = (uint32_t)IOSurfaceGetWidth(s);
	*out_h = (uint32_t)IOSurfaceGetHeight(s);
	return *out_w > 0 && *out_h > 0;
#else
	(void)iosurface;
	return false;
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
	const uint64_t size = (uint64_t)w * h * 4u;
	xrt_graphics_buffer_handle_t hb = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	struct ipc_cpu_buffer_map m;
	if (!ipc_cpu_buffer_create(size, &hb)) {
		return false;
	}
	if (!ipc_cpu_buffer_map(hb, size, true, &m)) {
		u_graphics_buffer_unref(&hb);
		return false;
	}
	const uint8_t *src = (const uint8_t *)bytes + (size_t)y * pitch + (size_t)x * 4u;
	for (uint32_t row = 0; row < h; row++) {
		memcpy((uint8_t *)m.ptr + (size_t)row * w * 4u, src + (size_t)row * pitch, (size_t)w * 4u);
	}
	ipc_cpu_buffer_unmap(&m);
	*out_handle = (void *)(intptr_t)hb;
	return true;
#endif
}

void *
oxr_conv_cpu_map(void *handle, uint64_t bytes, void **out_token)
{
	*out_token = NULL;
#ifdef XRT_OS_WINDOWS
	if (handle == NULL || bytes == 0) {
		return NULL;
	}
	return MapViewOfFile((HANDLE)handle, FILE_MAP_READ, 0, 0, (SIZE_T)bytes);
#else
	struct ipc_cpu_buffer_map *m = calloc(1, sizeof(*m));
	if (m == NULL) {
		return NULL;
	}
	if (!ipc_cpu_buffer_map((xrt_graphics_buffer_handle_t)(intptr_t)handle, bytes, false, m)) {
		free(m);
		return NULL;
	}
	*out_token = m;
	return m->ptr;
#endif
}

void
oxr_conv_cpu_unmap(void *view, void *token)
{
#ifdef XRT_OS_WINDOWS
	(void)token;
	if (view != NULL) {
		UnmapViewOfFile(view);
	}
#else
	(void)view;
	struct ipc_cpu_buffer_map *m = (struct ipc_cpu_buffer_map *)token;
	if (m != NULL) {
		ipc_cpu_buffer_unmap(m);
		free(m);
	}
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
	xrt_graphics_buffer_handle_t h = (xrt_graphics_buffer_handle_t)(intptr_t)handle;
	u_graphics_buffer_unref(&h); // an fd < 0 / NULL IOSurface is a no-op
#endif
}

#endif // OXR_HAVE_DXR_lift
