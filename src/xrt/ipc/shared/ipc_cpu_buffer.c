// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  CPU bytes over the IPC graphics-buffer transport (see ipc_cpu_buffer.h).
 * @ingroup ipc_shared
 */

#include "xrt/xrt_config_os.h"

#include "shared/ipc_cpu_buffer.h"

#include <stdio.h>
#include <string.h>

#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_FD) && defined(XRT_OS_LINUX) && !defined(XRT_OS_ANDROID)
#define IPC_CPU_BUFFER_FD 1
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_METAL)
#define IPC_CPU_BUFFER_IOSURFACE 1
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#endif

//! macOS: the section is ROW bytes wide, ceil(bytes / ROW) rows (rows are contiguous).
#define IPC_CPU_BUFFER_ROW 4096u

bool
ipc_cpu_buffer_supported(void)
{
#if defined(IPC_CPU_BUFFER_FD) || defined(IPC_CPU_BUFFER_IOSURFACE)
	return true;
#else
	return false;
#endif
}

#if defined(IPC_CPU_BUFFER_FD)

bool
ipc_cpu_buffer_create(uint64_t bytes, xrt_graphics_buffer_handle_t *out_handle)
{
	*out_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	if (bytes == 0 || bytes > ((uint64_t)1 << 32)) {
		return false;
	}
	// A unique name per call, unlinked at once: only the fd names the section.
	static int counter = 0;
	char name[48];
	const int id = __sync_fetch_and_add(&counter, 1);
	(void)snprintf(name, sizeof(name), "/dxr_cpu_%d_%d", (int)getpid(), id);
	const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		return false;
	}
	(void)shm_unlink(name);
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	if (ftruncate(fd, (off_t)bytes) != 0) {
		close(fd);
		return false;
	}
	*out_handle = fd;
	return true;
}

bool
ipc_cpu_buffer_map(xrt_graphics_buffer_handle_t handle, uint64_t bytes, bool writable, struct ipc_cpu_buffer_map *out)
{
	memset(out, 0, sizeof(*out));
	struct stat sb;
	if (handle < 0 || bytes == 0 || fstat(handle, &sb) != 0 || sb.st_size < 0 || (uint64_t)sb.st_size < bytes) {
		return false; // a short section would SIGBUS on access
	}
	void *p = mmap(NULL, (size_t)bytes, writable ? (PROT_READ | PROT_WRITE) : PROT_READ, MAP_SHARED, handle, 0);
	if (p == MAP_FAILED) {
		return false;
	}
	out->ptr = p;
	out->bytes = bytes;
	out->writable = writable;
	return true;
}

void
ipc_cpu_buffer_unmap(struct ipc_cpu_buffer_map *map)
{
	if (map == NULL || map->ptr == NULL) {
		return;
	}
	(void)munmap(map->ptr, (size_t)map->bytes);
	memset(map, 0, sizeof(*map));
}

#elif defined(IPC_CPU_BUFFER_IOSURFACE)

static CFNumberRef
num_i32(int32_t v)
{
	return CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
}

bool
ipc_cpu_buffer_create(uint64_t bytes, xrt_graphics_buffer_handle_t *out_handle)
{
	*out_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	if (bytes == 0 || bytes > ((uint64_t)1 << 32)) {
		return false;
	}
	const uint64_t rows = (bytes + IPC_CPU_BUFFER_ROW - 1) / IPC_CPU_BUFFER_ROW;
	// Global: the transport carries the bare IOSurface ID and the peer's
	// IOSurfaceLookup must find it (as for the weave output, #759).
	CFStringRef keys[5] = {CFSTR("IOSurfaceWidth"), CFSTR("IOSurfaceHeight"), CFSTR("IOSurfaceBytesPerElement"),
	                       CFSTR("IOSurfaceBytesPerRow"), CFSTR("IOSurfaceIsGlobal")};
	CFNumberRef nums[4] = {num_i32((int32_t)IPC_CPU_BUFFER_ROW), num_i32((int32_t)rows), num_i32(1),
	                       num_i32((int32_t)IPC_CPU_BUFFER_ROW)};
	CFTypeRef values[5] = {nums[0], nums[1], nums[2], nums[3], kCFBooleanTrue};
	CFDictionaryRef props = CFDictionaryCreate(NULL, (const void **)keys, (const void **)values, 5,
	                                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	for (int i = 0; i < 4; i++) {
		CFRelease(nums[i]);
	}
	if (props == NULL) {
		return false;
	}
	IOSurfaceRef s = IOSurfaceCreate(props);
	CFRelease(props);
	if (s == NULL) {
		return false;
	}
	if (IOSurfaceGetBytesPerRow(s) != IPC_CPU_BUFFER_ROW || IOSurfaceGetAllocSize(s) < bytes) {
		CFRelease(s); // padded rows would not be one contiguous byte range
		return false;
	}
	*out_handle = (xrt_graphics_buffer_handle_t)s;
	return true;
}

bool
ipc_cpu_buffer_map(xrt_graphics_buffer_handle_t handle, uint64_t bytes, bool writable, struct ipc_cpu_buffer_map *out)
{
	memset(out, 0, sizeof(*out));
	IOSurfaceRef s = (IOSurfaceRef)handle;
	if (s == NULL || bytes == 0 || CFGetTypeID(s) != IOSurfaceGetTypeID() ||
	    IOSurfaceGetBytesPerRow(s) * IOSurfaceGetHeight(s) < bytes || IOSurfaceGetAllocSize(s) < bytes) {
		return false;
	}
	const IOSurfaceLockOptions opts = writable ? 0 : kIOSurfaceLockReadOnly;
	if (IOSurfaceLock(s, opts, NULL) != 0 /* kIOReturnSuccess */) {
		return false;
	}
	void *p = IOSurfaceGetBaseAddress(s);
	if (p == NULL) {
		(void)IOSurfaceUnlock(s, opts, NULL);
		return false;
	}
	CFRetain(s);
	out->ptr = p;
	out->bytes = bytes;
	out->writable = writable;
	out->priv = (void *)s;
	return true;
}

void
ipc_cpu_buffer_unmap(struct ipc_cpu_buffer_map *map)
{
	if (map == NULL || map->ptr == NULL) {
		return;
	}
	IOSurfaceRef s = (IOSurfaceRef)map->priv;
	if (s != NULL) {
		(void)IOSurfaceUnlock(s, map->writable ? 0 : kIOSurfaceLockReadOnly, NULL);
		CFRelease(s);
	}
	memset(map, 0, sizeof(*map));
}

#else

bool
ipc_cpu_buffer_create(uint64_t bytes, xrt_graphics_buffer_handle_t *out_handle)
{
	(void)bytes;
	*out_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	return false;
}

bool
ipc_cpu_buffer_map(xrt_graphics_buffer_handle_t handle, uint64_t bytes, bool writable, struct ipc_cpu_buffer_map *out)
{
	(void)handle;
	(void)bytes;
	(void)writable;
	memset(out, 0, sizeof(*out));
	return false;
}

void
ipc_cpu_buffer_unmap(struct ipc_cpu_buffer_map *map)
{
	(void)map;
}

#endif
