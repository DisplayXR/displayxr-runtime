// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host: shared-memory sections by native handle.
 * @ingroup conv_host
 */

#include "conv_host_shm.h"

#include "xrt/xrt_config_os.h"
#include "os/os_process.h"

#include <stdio.h>

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

void
conv_host_handle_close(uint64_t h)
{
	if (h == 0 || h == OS_NATIVE_HANDLE_INVALID) {
		return;
	}
	os_native_handle_close(h);
}

#ifdef XRT_OS_WINDOWS

void *
conv_host_shm_map(uint64_t h, uint64_t bytes)
{
	return MapViewOfFile((HANDLE)(uintptr_t)h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, (SIZE_T)bytes);
}

void
conv_host_shm_unmap(void *p, uint64_t bytes)
{
	(void)bytes;
	if (p != NULL) {
		UnmapViewOfFile(p);
	}
}

uint64_t
conv_host_shm_create(uint64_t bytes, void **out_map)
{
	HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)(bytes >> 32),
	                              (DWORD)(bytes & 0xffffffffu), NULL);
	if (h == NULL) {
		return 0;
	}
	void *m = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, (SIZE_T)bytes);
	if (m == NULL) {
		CloseHandle(h);
		return 0;
	}
	*out_map = m;
	return (uint64_t)(uintptr_t)h;
}

#else

void *
conv_host_shm_map(uint64_t h, uint64_t bytes)
{
	void *p = mmap(NULL, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, (int)h, 0);
	return p == MAP_FAILED ? NULL : p;
}

void
conv_host_shm_unmap(void *p, uint64_t bytes)
{
	if (p != NULL) {
		munmap(p, (size_t)bytes);
	}
}

uint64_t
conv_host_shm_create(uint64_t bytes, void **out_map)
{
	char name[64];
	static unsigned counter = 0;
	(void)snprintf(name, sizeof(name), "/dxr-convhost-%ld-%u", (long)getpid(), __sync_add_and_fetch(&counter, 1));
	const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		return 0;
	}
	(void)shm_unlink(name);
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	if (ftruncate(fd, (off_t)bytes) != 0) {
		close(fd);
		return 0;
	}
	void *m = mmap(NULL, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) {
		close(fd);
		return 0;
	}
	*out_map = m;
	return (uint64_t)fd;
}

#endif
