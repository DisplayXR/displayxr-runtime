// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Present-hook re-entry detection, the Windows probe (#2635).
 *
 * Moved from comp_d3d11_target.cpp so the D3D12 target can share it.
 *
 * @ingroup comp_util
 */

#include "comp_present_hook_win.h"
#include "comp_present_hook.h"

#include "os/os_time.h"
#include "util/u_time.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <wchar.h>

static bool
reentry_readable(uint64_t addr, size_t len)
{
	MEMORY_BASIC_INFORMATION mbi = {};
	if (VirtualQuery(reinterpret_cast<const void *>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) {
		return false;
	}
	if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
		return false;
	}
	const uint64_t end = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
	return addr + len <= end;
}

static HMODULE
reentry_module_of(uint64_t addr)
{
	HMODULE m = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCWSTR>(addr), &m)) {
		return nullptr;
	}
	return m;
}

static bool
reentry_module_range(HMODULE m, struct comp_present_hook_range *out)
{
	const uint64_t base = reinterpret_cast<uint64_t>(m);
	if (m == nullptr || !reentry_readable(base, sizeof(IMAGE_DOS_HEADER))) {
		return false;
	}
	const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(m);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || !reentry_readable(base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS))) {
		return false;
	}
	const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		return false;
	}
	out->base = base;
	out->end = base + nt->OptionalHeader.SizeOfImage;
	return true;
}

/*!
 * Which module, other than the system DXGI, owns the code @p swapchain's
 * Present enters? Follows the vtable slot, then up to a few jump detours
 * (through trampolines in anonymous memory). nullptr = not hooked, or
 * a hook whose code lives in no module (nothing to match a stack against).
 */
static HMODULE
reentry_present_hook_owner(void *swapchain)
{
	if (swapchain == nullptr) {
		return nullptr;
	}
	const HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
	// IDXGISwapChain::Present = vtable slot 8 (IUnknown 3, IDXGIObject 4,
	// IDXGIDeviceSubObject 1).
	void **vtbl = *reinterpret_cast<void ***>(swapchain);
	uint64_t ip = reinterpret_cast<uint64_t>(vtbl[8]);
	for (int hop = 0; hop < 6 && ip != 0; hop++) {
		const HMODULE m = reentry_module_of(ip);
		if (m != nullptr && m != dxgi) {
			return m; // foreign code: the hook
		}
		if (!reentry_readable(ip, COMP_PRESENT_HOOK_DECODE_BYTES)) {
			return nullptr;
		}
		uint64_t next = 0;
		const enum comp_present_hook_jump j =
		    comp_present_hook_decode_jump(reinterpret_cast<const uint8_t *>(ip), ip, &next);
		if (j == COMP_PRESENT_HOOK_JUMP_NONE) {
			return nullptr; // DXGI's own code (or an unattributable stub)
		}
		if (j == COMP_PRESENT_HOOK_JUMP_INDIRECT) {
			if (!reentry_readable(next, sizeof(uint64_t))) {
				return nullptr;
			}
			next = comp_present_hook_rd64(reinterpret_cast<const uint8_t *>(next));
		}
		ip = next;
	}
	return nullptr;
}

extern "C" bool
comp_present_hook_win_sample(void *dxgi_swapchain,
                             bool *io_reentrant,
                             uint64_t *io_next_sample_ns,
                             char *out_module_name,
                             size_t module_name_size)
{
	if (*io_reentrant) {
		return false;
	}
	const uint64_t now = os_monotonic_get_ns();
	if (now < *io_next_sample_ns) {
		return false;
	}
	// An unhooked chain is re-checked once a second (a hook installed later
	// is rare). A hooked chain is checked on EVERY app present until it
	// latches: one present handed to another thread while the app sits in the
	// hook is enough to deadlock, so the check must never lag a present.
	const HMODULE owner = reentry_present_hook_owner(dxgi_swapchain);
	struct comp_present_hook_range range = {};
	if (owner == nullptr || !reentry_module_range(owner, &range)) {
		*io_next_sample_ns = now + U_TIME_1S_IN_NS;
		return false;
	}
	void *raw[62] = {};
	const USHORT n = RtlCaptureStackBackTrace(0, 62, raw, nullptr);
	uint64_t frames[62] = {};
	for (USHORT i = 0; i < n; i++) {
		frames[i] = reinterpret_cast<uint64_t>(raw[i]);
	}
	if (!comp_present_hook_stack_hits(frames, n, &range, 1)) {
		return false;
	}
	*io_reentrant = true;
	if (out_module_name != nullptr && module_name_size > 0) {
		snprintf(out_module_name, module_name_size, "?");
		wchar_t wpath[MAX_PATH] = {};
		if (GetModuleFileNameW(owner, wpath, MAX_PATH) != 0) {
			const wchar_t *base = wcsrchr(wpath, L'\\');
			WideCharToMultiByte(CP_UTF8, 0, base != nullptr ? base + 1 : wpath, -1, out_module_name,
			                    (int)module_name_size, nullptr, nullptr);
		}
	}
	return true;
}
