// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  os_shm.h on Windows: a named pagefile section with an explicit DACL.
 * @ingroup aux_os
 */

#include "os/os_shm.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>

#include <stdio.h>
#include <string.h>

static bool
to_wide(const char *in, wchar_t *out, int cap)
{
	return in != NULL && MultiByteToWideChar(CP_UTF8, 0, in, -1, out, cap) > 0;
}

/*!
 * SYSTEM, Administrators and the calling user's SID: full access; nobody
 * else. Medium mandatory label, no-write-up: a Medium client of the same
 * user can open it even when the creator runs elevated (whose default DACL
 * would admit Administrators only). The caller LocalFree()s the descriptor.
 */
static bool
make_security(SECURITY_ATTRIBUTES *sa)
{
	memset(sa, 0, sizeof(*sa));
	sa->nLength = sizeof(*sa);
	sa->bInheritHandle = FALSE;

	HANDLE tok = NULL;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
		return false;
	}
	DWORD_PTR buf[64]; // pointer-aligned for TOKEN_USER
	DWORD len = 0;
	const BOOL ok = GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len);
	CloseHandle(tok);
	if (!ok) {
		return false;
	}
	const TOKEN_USER *tu = (const TOKEN_USER *)buf;
	LPSTR sid = NULL;
	if (!ConvertSidToStringSidA(tu->User.Sid, &sid)) {
		return false;
	}
	char sddl[256];
	const int n = snprintf(sddl, sizeof(sddl), "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;%s)S:(ML;;NW;;;ME)", sid);
	LocalFree(sid);
	if (n <= 0 || (size_t)n >= sizeof(sddl)) {
		return false;
	}
	return ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl, SDDL_REVISION_1, &sa->lpSecurityDescriptor,
	                                                            NULL) != 0;
}

static int
map_handle(HANDLE h, const char *name, size_t size, bool writable, bool creator, struct os_shm *out)
{
	void *p = MapViewOfFile(h, writable ? (FILE_MAP_READ | FILE_MAP_WRITE) : FILE_MAP_READ, 0, 0, size);
	if (p == NULL) {
		return OS_SHM_ERR_FAILED;
	}
	out->map = p;
	out->size = size;
	out->native = (uint64_t)(uintptr_t)h;
	out->creator = creator;
	snprintf(out->name, sizeof(out->name), "%s", name);
	return 0;
}

int
os_shm_create(const char *name, size_t size, struct os_shm *out)
{
	if (out == NULL) {
		return OS_SHM_ERR_INVALID;
	}
	memset(out, 0, sizeof(*out));
	wchar_t wname[OS_SHM_NAME_MAX + 1];
	if (name == NULL || strlen(name) > OS_SHM_NAME_MAX || size == 0 || !to_wide(name, wname, OS_SHM_NAME_MAX + 1)) {
		return OS_SHM_ERR_INVALID;
	}
	SECURITY_ATTRIBUTES sa;
	const bool have_sa = make_security(&sa);
	const unsigned long long sz = (unsigned long long)size;
	HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, have_sa ? &sa : NULL, PAGE_READWRITE, (DWORD)(sz >> 32),
	                              (DWORD)(sz & 0xFFFFFFFFull), wname);
	const DWORD err = GetLastError();
	if (have_sa && sa.lpSecurityDescriptor != NULL) {
		LocalFree(sa.lpSecurityDescriptor);
	}
	if (h == NULL) {
		return OS_SHM_ERR_FAILED;
	}
	// ERROR_ALREADY_EXISTS: a previous creator's section kept alive by its
	// openers (a service that crashed while an app still reads the page). It
	// is adopted — the caller re-lays it out — because the alternative is no
	// page until every old reader exits. The name lives in this user's own
	// session namespace, so only this user (or an administrator) can have made it.
	(void)err;
	const int r = map_handle(h, name, size, true, true, out);
	if (r != 0) {
		CloseHandle(h);
	}
	return r;
}

int
os_shm_open(const char *name, size_t size, bool writable, struct os_shm *out)
{
	if (out == NULL) {
		return OS_SHM_ERR_INVALID;
	}
	memset(out, 0, sizeof(*out));
	wchar_t wname[OS_SHM_NAME_MAX + 1];
	if (name == NULL || strlen(name) > OS_SHM_NAME_MAX || size == 0 || !to_wide(name, wname, OS_SHM_NAME_MAX + 1)) {
		return OS_SHM_ERR_INVALID;
	}
	HANDLE h = OpenFileMappingW(writable ? (FILE_MAP_READ | FILE_MAP_WRITE) : FILE_MAP_READ, FALSE, wname);
	if (h == NULL) {
		return GetLastError() == ERROR_FILE_NOT_FOUND ? OS_SHM_ERR_NOT_FOUND : OS_SHM_ERR_FAILED;
	}
	const int r = map_handle(h, name, size, writable, false, out);
	if (r != 0) {
		CloseHandle(h);
		return r;
	}
	// A section smaller than asked: the view above would have failed, but be explicit.
	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery(out->map, &mbi, sizeof(mbi)) == 0 || mbi.RegionSize < size) {
		UnmapViewOfFile(out->map);
		CloseHandle(h);
		memset(out, 0, sizeof(*out));
		return OS_SHM_ERR_SHORT;
	}
	return 0;
}

void
os_shm_close(struct os_shm *shm)
{
	if (shm == NULL || shm->map == NULL) {
		return;
	}
	UnmapViewOfFile(shm->map);
	CloseHandle((HANDLE)(uintptr_t)shm->native);
	memset(shm, 0, sizeof(*shm));
}
