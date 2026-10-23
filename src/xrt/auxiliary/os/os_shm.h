// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Named shared memory: one creator, any number of openers found BY
 *         NAME (the viewer pose page, ADR-054 N8). Handle-passed sections go
 *         through ipc_shmem / os_process instead.
 *
 * Windows (os_shm_win32.c): a pagefile-backed `CreateFileMappingW` section in
 * the caller's `Local\` session namespace. The creator sets an explicit DACL —
 * SYSTEM, Administrators and the creating user's own SID get full access,
 * nobody else anything — plus a Medium mandatory label (no-write-up), so a
 * non-elevated client of the same user can open a page a service created even
 * when that service runs elevated (an elevated token's default DACL would
 * otherwise admit only Administrators). Openers use `OpenFileMappingW`.
 *
 * POSIX (os_shm_posix.c): `shm_open` with mode 0600 (owner only). The creator
 * unlinks a stale name first (a crashed creator's section) and creates with
 * O_EXCL; closing the creator's handle unlinks the name (openers keep their
 * mapping). macOS names are at most 31 bytes, `/` included.
 *
 * Android: not built (every call returns OS_SHM_ERR_UNSUPPORTED).
 *
 * This header includes no OS header.
 *
 * @ingroup aux_os
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! @name Errors @{ */
#define OS_SHM_ERR_FAILED (-1)      //!< the OS call failed
#define OS_SHM_ERR_INVALID (-2)     //!< bad argument
#define OS_SHM_ERR_UNSUPPORTED (-3) //!< not on this platform
#define OS_SHM_ERR_NOT_FOUND (-4)   //!< open: no section by that name
#define OS_SHM_ERR_SHORT (-5)       //!< open: the section is smaller than asked
/*! @} */

//! Longest name (NUL excluded) accepted on every platform.
#define OS_SHM_NAME_MAX 63

struct os_shm
{
	void *map;       //!< the mapping (NULL = not open)
	size_t size;     //!< mapped bytes
	uint64_t native; //!< Windows HANDLE / POSIX fd, widened
	bool creator;    //!< created by this process (POSIX: the name is unlinked at close)
	char name[OS_SHM_NAME_MAX + 1];
};

/*!
 * Create (or, on Windows, re-create) the section @p name of @p size bytes
 * and map it read-write. POSIX: a leftover name is unlinked first (old
 * openers keep the old section). Windows: an existing section of that name
 * (one a crashed creator's openers keep alive) is adopted — it lives in the
 * user's own `Local\` namespace. Either way the caller lays the memory out
 * again (it is not guaranteed zeroed).
 */
int
os_shm_create(const char *name, size_t size, struct os_shm *out);

/*!
 * Open an existing section @p name and map @p size bytes of it, read-write
 * when @p writable. OS_SHM_ERR_NOT_FOUND when there is none.
 */
int
os_shm_open(const char *name, size_t size, bool writable, struct os_shm *out);

//! Unmap and close (NULL- and closed-safe). The creator's close unlinks the POSIX name.
void
os_shm_close(struct os_shm *shm);

#ifdef __cplusplus
}
#endif
