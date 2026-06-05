// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Sanitized pre-load of plug-in dependency DLLs whose unwind data
 *         crashes host-engine module tracers (issue #434). See the header
 *         for the full mechanism write-up.
 * @ingroup target_common
 */

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_WINDOWS

#include "target_plugin_preload_sanitize.h"

#include "util/u_logging.h"

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Bounds for the import-graph walk. The walk is a startup-path best-effort
 * scan, not a loader reimplementation — generous caps keep a pathological
 * binary from turning xrCreateInstance into a disk crawl.
 */
#define SANITIZE_MAX_DEPTH 4
#define SANITIZE_MAX_VISITED 96
#define SANITIZE_MAX_IMPORTS_PER_MODULE 256
#define SANITIZE_MAX_BASENAME 96
#define SANITIZE_PATH_CAP 1024

// UNWIND_INFO flag bit (winnt.h only defines it for some target arches).
#define SANITIZE_UNW_FLAG_CHAININFO 0x4u


/*
 *
 * Small helpers.
 *
 */

struct sanitize_mapped_file
{
	HANDLE file;
	HANDLE mapping;
	const uint8_t *data;
	uint64_t size;
};

struct sanitize_visited
{
	wchar_t names[SANITIZE_MAX_VISITED][SANITIZE_MAX_BASENAME];
	int count;
};

static bool
sanitize_map_file(const wchar_t *path, struct sanitize_mapped_file *m)
{
	memset(m, 0, sizeof(*m));
	m->file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (m->file == INVALID_HANDLE_VALUE) {
		return false;
	}

	LARGE_INTEGER sz;
	if (!GetFileSizeEx(m->file, &sz) || sz.QuadPart <= 0) {
		CloseHandle(m->file);
		return false;
	}
	m->size = (uint64_t)sz.QuadPart;

	m->mapping = CreateFileMappingW(m->file, NULL, PAGE_READONLY, 0, 0, NULL);
	if (m->mapping == NULL) {
		CloseHandle(m->file);
		return false;
	}

	m->data = (const uint8_t *)MapViewOfFile(m->mapping, FILE_MAP_READ, 0, 0, 0);
	if (m->data == NULL) {
		CloseHandle(m->mapping);
		CloseHandle(m->file);
		return false;
	}
	return true;
}

static void
sanitize_unmap_file(struct sanitize_mapped_file *m)
{
	if (m->data != NULL) {
		UnmapViewOfFile(m->data);
	}
	if (m->mapping != NULL) {
		CloseHandle(m->mapping);
	}
	if (m->file != INVALID_HANDLE_VALUE && m->file != NULL) {
		CloseHandle(m->file);
	}
	memset(m, 0, sizeof(*m));
}

/*!
 * Validate DOS + NT headers of a mapped x64 PE file. Returns the NT headers
 * pointer or NULL when the file is not a loadable x64 image.
 */
static const IMAGE_NT_HEADERS64 *
sanitize_pe_headers(const uint8_t *d, uint64_t n)
{
	if (n < sizeof(IMAGE_DOS_HEADER)) {
		return NULL;
	}
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)d;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return NULL;
	}
	uint64_t nt_off = (uint64_t)(uint32_t)dos->e_lfanew;
	if (nt_off + sizeof(IMAGE_NT_HEADERS64) > n) {
		return NULL;
	}
	const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(d + nt_off);
	if (nt->Signature != IMAGE_NT_SIGNATURE ||                       //
	    nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||        //
	    nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) { //
		return NULL;
	}
	return nt;
}

/*!
 * Translate an RVA into a file-offset pointer using the section table.
 * Returns NULL when the RVA does not land inside any section's raw data
 * (so callers stay in-bounds on truncated or hostile images).
 */
static const uint8_t *
sanitize_rva_to_ptr(const uint8_t *d, uint64_t n, const IMAGE_NT_HEADERS64 *nt, uint32_t rva, uint32_t need)
{
	const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
	for (uint32_t i = 0; i < nt->FileHeader.NumberOfSections; i++) {
		uint32_t va = sec[i].VirtualAddress;
		uint32_t raw = sec[i].PointerToRawData;
		uint32_t raw_size = sec[i].SizeOfRawData;
		if (rva >= va && rva + need <= va + raw_size) {
			uint64_t off = (uint64_t)raw + (rva - va);
			if (off + need > n) {
				return NULL;
			}
			return d + off;
		}
	}
	return NULL;
}

static bool
sanitize_pe_data_dir(const IMAGE_NT_HEADERS64 *nt, uint32_t idx, uint32_t *out_rva, uint32_t *out_size)
{
	if (idx >= nt->OptionalHeader.NumberOfRvaAndSizes) {
		return false;
	}
	const IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[idx];
	if (dir->VirtualAddress == 0 || dir->Size == 0) {
		return false;
	}
	*out_rva = dir->VirtualAddress;
	*out_size = dir->Size;
	return true;
}

/*!
 * The precise trigger shape for UE's `FBacktracer::AddModule` cursor
 * underflow: the module's FIRST version-1 UNWIND_INFO is chained
 * (UNW_FLAG_CHAININFO). Entries with other version values are skipped by
 * that parser without advancing its output cursor, so a leading chained v1
 * entry makes it write before the start of its own allocation.
 *
 * Checking the exact precondition (rather than name-matching DLLs) keeps
 * this vendor-agnostic and self-retiring: a rebuilt dependency with normal
 * unwind data stops matching and is loaded the ordinary way again.
 */
static bool
sanitize_unwind_shape_is_tracer_unsafe(const uint8_t *d, uint64_t n, const IMAGE_NT_HEADERS64 *nt)
{
	uint32_t rva = 0;
	uint32_t size = 0;
	if (!sanitize_pe_data_dir(nt, IMAGE_DIRECTORY_ENTRY_EXCEPTION, &rva, &size)) {
		return false;
	}

	const uint32_t entry_size = (uint32_t)sizeof(RUNTIME_FUNCTION); // 12 on x64
	uint32_t count = size / entry_size;
	const uint8_t *table = sanitize_rva_to_ptr(d, n, nt, rva, count * entry_size);
	if (table == NULL || count == 0) {
		return false;
	}

	for (uint32_t i = 0; i < count; i++) {
		const RUNTIME_FUNCTION *fn = (const RUNTIME_FUNCTION *)(table + (uint64_t)i * entry_size);
		// Low bit set means the entry chains to another RUNTIME_FUNCTION
		// rather than UNWIND_INFO; mask it off like the OS unwinder does.
		const uint8_t *ui = sanitize_rva_to_ptr(d, n, nt, fn->UnwindInfoAddress & ~1u, 1);
		if (ui == NULL) {
			continue;
		}
		uint8_t version = *ui & 0x7;
		uint8_t flags = *ui >> 3;
		if (version == 1) {
			return (flags & SANITIZE_UNW_FLAG_CHAININFO) != 0;
		}
	}
	return false;
}


/*
 *
 * Dependency resolution.
 *
 */

static bool
sanitize_visited_add(struct sanitize_visited *v, const wchar_t *basename)
{
	for (int i = 0; i < v->count; i++) {
		if (_wcsicmp(v->names[i], basename) == 0) {
			return false; // already seen
		}
	}
	if (v->count >= SANITIZE_MAX_VISITED) {
		return false;
	}
	wcsncpy(v->names[v->count], basename, SANITIZE_MAX_BASENAME - 1);
	v->names[v->count][SANITIZE_MAX_BASENAME - 1] = L'\0';
	v->count++;
	return true;
}

/*!
 * Resolve an import name to an on-disk path worth scanning. Mirrors (an
 * approximation of) the loader search the plug-in load will perform:
 * importing module's dir → plug-in dir → standard search path. Returns
 * false for api-sets, already-resident modules, system DLLs, and anything
 * that does not resolve — none of those need (or can get) sanitizing.
 */
static bool
sanitize_resolve_dependency(const char *name8,
                            const wchar_t *importer_dir,
                            const wchar_t *plugin_dir,
                            wchar_t *out_path,
                            size_t out_cap,
                            wchar_t *out_basename,
                            size_t basename_cap)
{
	wchar_t name[SANITIZE_MAX_BASENAME];
	int n = MultiByteToWideChar(CP_UTF8, 0, name8, -1, name, SANITIZE_MAX_BASENAME);
	if (n <= 0) {
		return false;
	}

	if (_wcsnicmp(name, L"api-ms-", 7) == 0 || _wcsnicmp(name, L"ext-ms-", 7) == 0) {
		return false; // api-set, resolved by the OS schema
	}

	if (GetModuleHandleW(name) != NULL) {
		return false; // already resident — imports will bind to it as-is
	}

	wchar_t candidate[SANITIZE_PATH_CAP];
	const wchar_t *dirs[2] = {importer_dir, plugin_dir};
	bool found = false;
	for (int i = 0; i < 2 && !found; i++) {
		if (dirs[i] == NULL || dirs[i][0] == L'\0') {
			continue;
		}
		if (swprintf_s(candidate, SANITIZE_PATH_CAP, L"%s\\%s", dirs[i], name) <= 0) {
			continue;
		}
		if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES) {
			found = true;
		}
	}
	if (!found) {
		DWORD len = SearchPathW(NULL, name, NULL, SANITIZE_PATH_CAP, candidate, NULL);
		if (len == 0 || len >= SANITIZE_PATH_CAP) {
			return false;
		}
		found = true;
	}

	// System DLLs are toolchain-standard; scanning them is wasted work.
	wchar_t sysdir[MAX_PATH];
	UINT sn = GetSystemDirectoryW(sysdir, MAX_PATH);
	if (sn > 0 && sn < MAX_PATH && _wcsnicmp(candidate, sysdir, sn) == 0) {
		return false;
	}

	wcsncpy(out_path, candidate, out_cap - 1);
	out_path[out_cap - 1] = L'\0';
	wcsncpy(out_basename, name, basename_cap - 1);
	out_basename[basename_cap - 1] = L'\0';
	return true;
}


/*
 *
 * Staging + pre-load.
 *
 */

/*!
 * Copy @p src into a per-user staging dir whose path contains the literal
 * "ThirdParty" segment (the engine-side filter is case-sensitive). The
 * version-keyed subdir (file size + mtime) sidesteps locked-file replace
 * and keeps the staged copy in lockstep with vendor-platform auto-updates;
 * stale sibling versions are garbage-collected best-effort.
 */
static bool
sanitize_stage_copy(const wchar_t *src, const wchar_t *basename, wchar_t *out_staged, size_t out_cap)
{
	wchar_t local[SANITIZE_PATH_CAP];
	DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, SANITIZE_PATH_CAP);
	if (n == 0 || n >= SANITIZE_PATH_CAP) {
		return false;
	}

	WIN32_FILE_ATTRIBUTE_DATA fad;
	if (!GetFileAttributesExW(src, GetFileExInfoStandard, &fad)) {
		return false;
	}

	wchar_t name_dir[SANITIZE_PATH_CAP];
	if (swprintf_s(name_dir, SANITIZE_PATH_CAP, L"%s\\DisplayXR\\ThirdParty\\%s", local, basename) <= 0) {
		return false;
	}

	wchar_t ver_dir[SANITIZE_PATH_CAP];
	if (swprintf_s(ver_dir, SANITIZE_PATH_CAP, L"%s\\%08lx%08lx_%08lx%08lx", name_dir,
	               (unsigned long)fad.nFileSizeHigh, (unsigned long)fad.nFileSizeLow,
	               (unsigned long)fad.ftLastWriteTime.dwHighDateTime,
	               (unsigned long)fad.ftLastWriteTime.dwLowDateTime) <= 0) {
		return false;
	}

	wchar_t staged[SANITIZE_PATH_CAP];
	if (swprintf_s(staged, SANITIZE_PATH_CAP, L"%s\\%s", ver_dir, basename) <= 0) {
		return false;
	}

	// Create the directory chain (each level may already exist).
	wchar_t partial[SANITIZE_PATH_CAP];
	const wchar_t *levels[4];
	int level_count = 0;
	{
		// %LOCALAPPDATA%\DisplayXR, ...\ThirdParty, ...\<name>, ...\<ver>
		static const wchar_t *suffixes[] = {L"\\DisplayXR", L"\\DisplayXR\\ThirdParty"};
		for (int i = 0; i < 2; i++) {
			if (swprintf_s(partial, SANITIZE_PATH_CAP, L"%s%s", local, suffixes[i]) > 0) {
				CreateDirectoryW(partial, NULL);
			}
		}
		levels[level_count++] = name_dir;
		levels[level_count++] = ver_dir;
	}
	for (int i = 0; i < level_count; i++) {
		CreateDirectoryW(levels[i], NULL);
	}

	WIN32_FILE_ATTRIBUTE_DATA staged_fad;
	bool fresh = GetFileAttributesExW(staged, GetFileExInfoStandard, &staged_fad) &&
	             staged_fad.nFileSizeLow == fad.nFileSizeLow && staged_fad.nFileSizeHigh == fad.nFileSizeHigh;
	if (!fresh && !CopyFileW(src, staged, FALSE)) {
		U_LOG_W("plugin loader: sanitized pre-load: copy of %ls failed (err=%lu) — loading normally.", src,
		        GetLastError());
		return false;
	}

	// Best-effort GC of stale version dirs (in-use ones simply stay).
	{
		wchar_t pattern[SANITIZE_PATH_CAP];
		if (swprintf_s(pattern, SANITIZE_PATH_CAP, L"%s\\*", name_dir) > 0) {
			WIN32_FIND_DATAW fd;
			HANDLE find = FindFirstFileW(pattern, &fd);
			if (find != INVALID_HANDLE_VALUE) {
				do {
					if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
					    wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
						continue;
					}
					wchar_t stale_dir[SANITIZE_PATH_CAP];
					if (swprintf_s(stale_dir, SANITIZE_PATH_CAP, L"%s\\%s", name_dir,
					               fd.cFileName) <= 0 ||
					    wcscmp(stale_dir, ver_dir) == 0) {
						continue;
					}
					wchar_t stale_file[SANITIZE_PATH_CAP];
					if (swprintf_s(stale_file, SANITIZE_PATH_CAP, L"%s\\%s", stale_dir, basename) >
					    0) {
						DeleteFileW(stale_file);
					}
					RemoveDirectoryW(stale_dir);
				} while (FindNextFileW(find, &fd));
				FindClose(find);
			}
		}
	}

	wcsncpy(out_staged, staged, out_cap - 1);
	out_staged[out_cap - 1] = L'\0';
	return true;
}


/*
 *
 * Import-graph walk.
 *
 */

static void
sanitize_scan_module(
    const wchar_t *path, const wchar_t *plugin_dir, int depth, bool is_root, struct sanitize_visited *v);

/*!
 * Handle one resolved dependency: recurse into its imports first (so a
 * vulnerable grand-dependency is sanitized before anything pulls it in),
 * then shape-check and sanitize-preload the dependency itself.
 */
static void
sanitize_handle_dependency(const wchar_t *dep_path,
                           const wchar_t *dep_basename,
                           const wchar_t *plugin_dir,
                           int depth,
                           struct sanitize_visited *v)
{
	sanitize_scan_module(dep_path, plugin_dir, depth + 1, false, v);

	struct sanitize_mapped_file m;
	if (!sanitize_map_file(dep_path, &m)) {
		return;
	}
	const IMAGE_NT_HEADERS64 *nt = sanitize_pe_headers(m.data, m.size);
	bool unsafe = nt != NULL && sanitize_unwind_shape_is_tracer_unsafe(m.data, m.size, nt);
	sanitize_unmap_file(&m);
	if (!unsafe) {
		return;
	}

	wchar_t staged[SANITIZE_PATH_CAP];
	if (!sanitize_stage_copy(dep_path, dep_basename, staged, SANITIZE_PATH_CAP)) {
		return;
	}

	if (LoadLibraryExW(staged, NULL, LOAD_WITH_ALTERED_SEARCH_PATH) == NULL) {
		U_LOG_W("plugin loader: sanitized pre-load of %ls failed (err=%lu) — loading normally.", staged,
		        GetLastError());
		return;
	}

	U_LOG_W(
	    "plugin loader: sanitized pre-load: '%ls' has tracer-unsafe unwind data (leading chained v1 "
	    "entry, issue #434) — pre-loaded from %ls so host module tracers skip it.",
	    dep_basename, staged);
}

/*!
 * Iterate one module's regular + delay import names, resolving and handling
 * each. Delay imports are included because the host's tracer fires whenever
 * the DLL eventually loads — eagerly pre-loading a vulnerable one merely
 * moves a load that was already going to happen.
 */
static void
sanitize_scan_module(
    const wchar_t *path, const wchar_t *plugin_dir, int depth, bool is_root, struct sanitize_visited *v)
{
	if (depth > SANITIZE_MAX_DEPTH) {
		return;
	}

	struct sanitize_mapped_file m;
	if (!sanitize_map_file(path, &m)) {
		return;
	}
	const IMAGE_NT_HEADERS64 *nt = sanitize_pe_headers(m.data, m.size);
	if (nt == NULL) {
		sanitize_unmap_file(&m);
		return;
	}

	if (is_root && sanitize_unwind_shape_is_tracer_unsafe(m.data, m.size, nt)) {
		// The plug-in binary's registered path IS its identity (vendors
		// resolve resources relative to it) — never re-stage it, just
		// surface the problem so the vendor can rebuild.
		U_LOG_W(
		    "plugin loader: plug-in binary %ls itself has tracer-unsafe unwind data (issue #434) — "
		    "cannot sanitize the registered binary; host engines with module tracers may crash.",
		    path);
	}

	wchar_t importer_dir[SANITIZE_PATH_CAP];
	wcsncpy(importer_dir, path, SANITIZE_PATH_CAP - 1);
	importer_dir[SANITIZE_PATH_CAP - 1] = L'\0';
	wchar_t *slash = wcsrchr(importer_dir, L'\\');
	if (slash != NULL) {
		*slash = L'\0';
	} else {
		importer_dir[0] = L'\0';
	}

	// Collect names from both import directories before recursing, so the
	// mapping isn't held across the whole subtree walk.
	char names[SANITIZE_MAX_IMPORTS_PER_MODULE][SANITIZE_MAX_BASENAME];
	int name_count = 0;

	uint32_t rva = 0;
	uint32_t size = 0;
	if (sanitize_pe_data_dir(nt, IMAGE_DIRECTORY_ENTRY_IMPORT, &rva, &size)) {
		for (uint32_t off = 0; name_count < SANITIZE_MAX_IMPORTS_PER_MODULE;
		     off += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
			const IMAGE_IMPORT_DESCRIPTOR *imp = (const IMAGE_IMPORT_DESCRIPTOR *)sanitize_rva_to_ptr(
			    m.data, m.size, nt, rva + off, sizeof(IMAGE_IMPORT_DESCRIPTOR));
			if (imp == NULL || imp->Name == 0) {
				break;
			}
			const char *nm = (const char *)sanitize_rva_to_ptr(m.data, m.size, nt, imp->Name, 2);
			if (nm != NULL) {
				strncpy(names[name_count], nm, SANITIZE_MAX_BASENAME - 1);
				names[name_count][SANITIZE_MAX_BASENAME - 1] = '\0';
				name_count++;
			}
		}
	}
	if (sanitize_pe_data_dir(nt, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT, &rva, &size)) {
		for (uint32_t off = 0; name_count < SANITIZE_MAX_IMPORTS_PER_MODULE;
		     off += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)) {
			const IMAGE_DELAYLOAD_DESCRIPTOR *imp = (const IMAGE_DELAYLOAD_DESCRIPTOR *)sanitize_rva_to_ptr(
			    m.data, m.size, nt, rva + off, sizeof(IMAGE_DELAYLOAD_DESCRIPTOR));
			if (imp == NULL || imp->DllNameRVA == 0) {
				break;
			}
			const char *nm = (const char *)sanitize_rva_to_ptr(m.data, m.size, nt, imp->DllNameRVA, 2);
			if (nm != NULL) {
				strncpy(names[name_count], nm, SANITIZE_MAX_BASENAME - 1);
				names[name_count][SANITIZE_MAX_BASENAME - 1] = '\0';
				name_count++;
			}
		}
	}

	sanitize_unmap_file(&m);

	for (int i = 0; i < name_count; i++) {
		wchar_t dep_path[SANITIZE_PATH_CAP];
		wchar_t dep_base[SANITIZE_MAX_BASENAME];
		if (!sanitize_resolve_dependency(names[i], importer_dir, plugin_dir, dep_path, SANITIZE_PATH_CAP,
		                                 dep_base, SANITIZE_MAX_BASENAME)) {
			continue;
		}
		if (!sanitize_visited_add(v, dep_base)) {
			continue;
		}
		sanitize_handle_dependency(dep_path, dep_base, plugin_dir, depth, v);
	}
}

void
target_plugin_sanitized_preload(const wchar_t *plugin_binary_path)
{
	if (plugin_binary_path == NULL || plugin_binary_path[0] == L'\0') {
		return;
	}

	wchar_t plugin_dir[SANITIZE_PATH_CAP];
	wcsncpy(plugin_dir, plugin_binary_path, SANITIZE_PATH_CAP - 1);
	plugin_dir[SANITIZE_PATH_CAP - 1] = L'\0';
	wchar_t *slash = wcsrchr(plugin_dir, L'\\');
	if (slash != NULL) {
		*slash = L'\0';
	} else {
		plugin_dir[0] = L'\0';
	}

	struct sanitize_visited visited;
	memset(&visited, 0, sizeof(visited));

	sanitize_scan_module(plugin_binary_path, plugin_dir, 0, true, &visited);
}

#else // !XRT_OS_WINDOWS

// The host-tracer bug this works around is Windows-only (x64 unwind-table
// parsing in loader notification callbacks); keep the TU non-empty.
typedef int target_plugin_preload_sanitize_not_used_on_this_platform;

#endif // XRT_OS_WINDOWS
