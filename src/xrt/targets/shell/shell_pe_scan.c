// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  PE import table scanner implementation.
 *
 * Uses LoadLibraryEx(LOAD_LIBRARY_AS_DATAFILE) + Dbghelp ImageDirectoryEntryToData
 * to walk the import directory without executing any code from the target binary.
 *
 * @ingroup shell
 */

#include "shell_pe_scan.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>

static int
stricmp_safe(const char *a, const char *b)
{
	if (!a || !b) return a == b ? 0 : (a ? 1 : -1);
	while (*a && *b) {
		int ca = (unsigned char)*a;
		int cb = (unsigned char)*b;
		if (ca >= 'A' && ca <= 'Z') ca += 32;
		if (cb >= 'A' && cb <= 'Z') cb += 32;
		if (ca != cb) return ca - cb;
		a++; b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

bool
shell_pe_exe_imports(const char *exe_path, const char *dll_name)
{
	if (!exe_path || !dll_name || !*exe_path || !*dll_name) {
		return false;
	}

	// LOAD_LIBRARY_AS_IMAGE_RESOURCE maps the file as an image (sections placed
	// at their VAs) without resolving imports or running DllMain — safe for
	// untrusted binaries, and lets ImageDirectoryEntryToData treat the base as
	// a mapped image (MappedAsImage=TRUE). The returned HMODULE has the low
	// bits set to mark it as a resource mapping; mask them off to get a usable
	// image base.
	HMODULE mod = LoadLibraryExA(exe_path, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
	if (!mod) {
		return false;
	}

	uintptr_t base = (uintptr_t)mod & ~(uintptr_t)0xF;

	bool found = false;

	ULONG dir_size = 0;
	PIMAGE_IMPORT_DESCRIPTOR import_dir = (PIMAGE_IMPORT_DESCRIPTOR)ImageDirectoryEntryToData(
	    (PVOID)base, TRUE, IMAGE_DIRECTORY_ENTRY_IMPORT, &dir_size);

	if (import_dir && dir_size > 0) {
		for (PIMAGE_IMPORT_DESCRIPTOR desc = import_dir;
		     desc->Name != 0 || desc->FirstThunk != 0;
		     desc++) {

			const char *imported = (const char *)(base + desc->Name);
			// Compare basename only (no path separator expected in PE import names,
			// but tolerate "./foo.dll" just in case).
			const char *slash = strrchr(imported, '\\');
			if (!slash) slash = strrchr(imported, '/');
			const char *base_name = slash ? slash + 1 : imported;

			if (stricmp_safe(base_name, dll_name) == 0) {
				found = true;
				break;
			}
		}
	}

	FreeLibrary(mod);
	return found;
}

#else // !_WIN32

bool
shell_pe_exe_imports(const char *exe_path, const char *dll_name)
{
	(void)exe_path;
	(void)dll_name;
	return false;
}

#endif
