// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host: shared-memory sections addressed by a
 *         native handle (Windows file mapping / POSIX fd), the one OS piece
 *         the data lanes need that os/os_process.h does not cover.
 *
 * Handles are os_process.h native handles (u64). "No handle" is 0 or
 * OS_NATIVE_HANDLE_INVALID; @ref conv_host_handle_close ignores both, so a
 * zeroed field never closes POSIX fd 0.
 *
 * @ingroup conv_host
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Close a native handle; 0 and OS_NATIVE_HANDLE_INVALID are ignored.
void
conv_host_handle_close(uint64_t h);

//! Map @p bytes of the section @p h read + write. NULL on failure.
void *
conv_host_shm_map(uint64_t h, uint64_t bytes);

void
conv_host_shm_unmap(void *p, uint64_t bytes);

//! Create a section of @p bytes, mapped at *@p out_map. Returns its handle (0 on failure).
uint64_t
conv_host_shm_create(uint64_t bytes, void **out_map);

#ifdef __cplusplus
}
#endif
