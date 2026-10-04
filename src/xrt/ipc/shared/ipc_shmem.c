// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  IPC shared memory helpers
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup ipc_shared
 */

#include <xrt/xrt_config_os.h>

#include "shared/ipc_shmem.h"

#include <stdio.h>

#if defined(XRT_OS_UNIX)
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(XRT_OS_ANDROID)
#include <android/sharedmem.h>
#elif defined(XRT_OS_UNIX)
// non-android unix
#include <sys/stat.h>
#include <fcntl.h>
#endif

#if defined(XRT_OS_ANDROID)

#if __ANDROID_API__ < 26
#error "Android API level 26 or higher needed for ASharedMemory_create"
#endif
xrt_result_t

ipc_shmem_create(size_t size, xrt_shmem_handle_t *out_handle, void **out_map)
{

	int fd = ASharedMemory_create("monado", size);
	if (fd < 0) {
		return XRT_ERROR_IPC_FAILURE;
	}
	xrt_result_t result = ipc_shmem_map(fd, size, out_map);
	if (result != XRT_SUCCESS) {
		close(fd);
		return result;
	}
	*out_handle = fd;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_shmem_create_with_readonly(size_t size,
                               xrt_shmem_handle_t *out_handle,
                               void **out_map,
                               xrt_shmem_handle_t *out_ro)
{
	*out_ro = -1;
	xrt_result_t result = ipc_shmem_create(size, out_handle, out_map);
	if (result != XRT_SUCCESS) {
		return result;
	}
	int ro = dup(*out_handle);
	if (ro < 0 || ASharedMemory_setProt(ro, PROT_READ) != 0) {
		if (ro >= 0) {
			close(ro);
		}
		ipc_shmem_destroy(out_handle, out_map, size);
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_ro = ro;
	return XRT_SUCCESS;
}

#elif defined(XRT_OS_UNIX)

#define MONADO_SHMEM_NAME "/displayxr_shm"
// Impl for non-Android Unix.
xrt_result_t
ipc_shmem_create(size_t size, xrt_shmem_handle_t *out_handle, void **out_map)
{
	*out_handle = -1;
	int fd = shm_open(MONADO_SHMEM_NAME, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		return XRT_ERROR_IPC_FAILURE;
	}

	if (ftruncate(fd, size) < 0) {
		close(fd);
		return XRT_ERROR_IPC_FAILURE;
	}
	xrt_result_t result = ipc_shmem_map(fd, size, out_map);
	if (result != XRT_SUCCESS) {
		close(fd);
		return result;
	}

	// Don't need the name entry anymore, we can share the FD.
	shm_unlink(MONADO_SHMEM_NAME);
	*out_handle = fd;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_shmem_create_with_readonly(size_t size,
                               xrt_shmem_handle_t *out_handle,
                               void **out_map,
                               xrt_shmem_handle_t *out_ro)
{
	*out_handle = -1;
	*out_ro = -1;
	// A unique name per call: the read-only fd must be opened by name before
	// the name is unlinked, and two threads may be here at once.
	static int counter = 0;
	char name[48];
	int id = __sync_fetch_and_add(&counter, 1);
	snprintf(name, sizeof(name), "/dxr_shm_%d_%d", (int)getpid(), id);
	int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		return XRT_ERROR_IPC_FAILURE;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		shm_unlink(name);
		return XRT_ERROR_IPC_FAILURE;
	}
	int ro = shm_open(name, O_RDONLY, 0);
	shm_unlink(name);
	if (ro < 0) {
		close(fd);
		return XRT_ERROR_IPC_FAILURE;
	}
	xrt_result_t result = ipc_shmem_map(fd, size, out_map);
	if (result != XRT_SUCCESS) {
		close(fd);
		close(ro);
		return result;
	}
	*out_handle = fd;
	*out_ro = ro;
	return XRT_SUCCESS;
}

#elif defined(XRT_OS_WINDOWS)

#include "util/u_windows.h"

#include <sddl.h>

static BOOL
create_appcontainer_security_attributes(SECURITY_ATTRIBUTES *sa)
{
	sa->nLength = sizeof(SECURITY_ATTRIBUTES);
	sa->bInheritHandle = FALSE;
	sa->lpSecurityDescriptor = NULL;

	// Same SDDL as named pipes - allows AppContainer access for Chrome WebXR
	const char *sddl =                   //
	    "D:"                             // Discretionary ACL
	    "(D;OICI;GA;;;BG)"               // Guest: deny
	    "(D;OICI;GA;;;AN)"               // Anonymous: deny
	    "(A;OICI;GRGWGX;;;AC)"           // UWP/AppContainer: read/write/execute
	    "(A;OICI;GRGWGX;;;AU)"           // Authenticated user: read/write/execute
	    "(A;OICI;GA;;;BA)";              // Administrator: full control

	return ConvertStringSecurityDescriptorToSecurityDescriptorA(
	    sddl, SDDL_REVISION_1, &sa->lpSecurityDescriptor, NULL);
}

xrt_result_t
ipc_shmem_create(size_t size, xrt_shmem_handle_t *out_handle, void **out_map)
{
	*out_handle = NULL;

	SECURITY_ATTRIBUTES sa;
	LPSECURITY_ATTRIBUTES lpsa = NULL;
	if (create_appcontainer_security_attributes(&sa)) {
		lpsa = &sa;
	}

	LARGE_INTEGER sz = {.QuadPart = size};
	HANDLE handle = CreateFileMappingA(INVALID_HANDLE_VALUE, lpsa, PAGE_READWRITE, sz.HighPart, sz.LowPart, NULL);

	if (sa.lpSecurityDescriptor != NULL) {
		LocalFree(sa.lpSecurityDescriptor);
	}

	if (handle == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}

	xrt_result_t result = ipc_shmem_map(handle, size, out_map);
	if (result != XRT_SUCCESS) {
		CloseHandle(handle);
		return result;
	}

	*out_handle = handle;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_shmem_create_with_readonly(size_t size,
                               xrt_shmem_handle_t *out_handle,
                               void **out_map,
                               xrt_shmem_handle_t *out_ro)
{
	*out_ro = NULL;
	xrt_result_t result = ipc_shmem_create(size, out_handle, out_map);
	if (result != XRT_SUCCESS) {
		return result;
	}
	HANDLE ro = NULL;
	if (!DuplicateHandle(GetCurrentProcess(), *out_handle, GetCurrentProcess(), &ro, FILE_MAP_READ, FALSE, 0)) {
		ipc_shmem_destroy(out_handle, out_map, size);
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_ro = ro;
	return XRT_SUCCESS;
}

#else
#error "OS not yet supported"
#endif

#if defined(XRT_OS_UNIX)

void
ipc_shmem_destroy(xrt_shmem_handle_t *handle_ptr, void **map_ptr, size_t size)
{
	// Checks for NULL.
	ipc_shmem_unmap((void **)map_ptr, size);

	if (handle_ptr == NULL) {
		return;
	}
	xrt_shmem_handle_t handle = *handle_ptr;
	if (handle < 0) {
		return;
	}
	close(handle);
	*handle_ptr = -1;
}

void
ipc_shmem_close_handle(xrt_shmem_handle_t *handle_ptr)
{
	if (handle_ptr == NULL || *handle_ptr < 0) {
		return;
	}
	close(*handle_ptr);
	*handle_ptr = -1;
}

xrt_result_t
ipc_shmem_map(xrt_shmem_handle_t handle, size_t size, void **out_map)
{

	const int access = PROT_READ | PROT_WRITE;
	const int flags = MAP_SHARED;
	void *ptr = mmap(NULL, size, access, flags, handle, 0);
	if (ptr == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_map = ptr;
	return XRT_SUCCESS;
}

void
ipc_shmem_unmap(void **map_ptr, size_t size)
{
	if (map_ptr == NULL) {
		return;
	}
	munmap(*map_ptr, size);
	*map_ptr = NULL;
}

#elif defined(XRT_OS_WINDOWS)

void
ipc_shmem_destroy(xrt_shmem_handle_t *handle_ptr, void **map_ptr, size_t size)
{
	// Checks for NULL.
	ipc_shmem_unmap((void **)map_ptr, size);

	if (handle_ptr == NULL) {
		return;
	}
	xrt_shmem_handle_t handle = *handle_ptr;
	CloseHandle(handle);
	*handle_ptr = NULL;
}

void
ipc_shmem_close_handle(xrt_shmem_handle_t *handle_ptr)
{
	if (handle_ptr == NULL || *handle_ptr == NULL) {
		return;
	}
	CloseHandle(*handle_ptr);
	*handle_ptr = NULL;
}

xrt_result_t
ipc_shmem_map(xrt_shmem_handle_t handle, size_t size, void **out_map)
{
	void *ptr = MapViewOfFile(handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
	if (ptr == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_map = ptr;
	return XRT_SUCCESS;
}

void
ipc_shmem_unmap(void **map_ptr, size_t size)
{
	if (map_ptr == NULL) {
		return;
	}
	void *map = *map_ptr;
	if (map == NULL) {
		return;
	}
	UnmapViewOfFile(map);
	*map_ptr = NULL;
}

#else
#error "OS not yet supported"
#endif
