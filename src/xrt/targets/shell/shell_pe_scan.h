// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  PE import table scanner — check if a Windows executable imports a given DLL.
 *
 * Used by the shell's app discovery scanner to sanity-check that a sidecar-declared
 * DisplayXR app actually links against openxr_loader.dll.
 *
 * @ingroup shell
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Returns true if the PE image at @p exe_path has @p dll_name in its import
 * directory. The comparison is case-insensitive on the DLL basename.
 *
 * Returns false on any parse error, if the file is not a valid PE, or if the
 * import is not present.
 *
 * Windows only. On non-Windows builds this is a no-op that always returns false.
 */
bool
shell_pe_exe_imports(const char *exe_path, const char *dll_name);

#ifdef __cplusplus
}
#endif
