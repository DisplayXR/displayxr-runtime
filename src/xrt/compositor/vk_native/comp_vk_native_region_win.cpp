// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-owned window region, applied right after the present it was
 *         cut from. See comp_vk_native_region_win.h for the why.
 */

#include "comp_vk_native_region_win.h"

#include "util/u_logging.h"
#include "os/os_time.h"

#include <windows.h>
#include <psapi.h> // K32EnumProcessModules (kernel32 export, no psapi.lib)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

//! Window px per coverage texel, and the dilation in texels — the shipping
//! defaults of dxr::ClickThroughRegion, so hit-testing feels the same.
constexpr uint32_t kTexel = 4;
constexpr uint32_t kDilate = 1;
//! Presented app frames unioned into one region. A frame can still be on
//! glass up to two presents after a newer one was queued (bridge latency 1 +
//! the frame DWM is scanning), and a union only ever adds pixels the frame
//! leaves transparent — which look identical inside and outside the region.
constexpr uint32_t kHistory = 3;

typedef int(WINAPI *PFN_SetWindowRgn)(HWND, HRGN, BOOL);
PFN_SetWindowRgn g_real = nullptr;

struct RegionState
{
	HWND hwnd = nullptr;
	//! The app asked for a shaped window (non-NULL SetWindowRgn) and has not
	//! unshaped it since.
	std::atomic<bool> app_shaped{false};
	//! The runtime has applied a region since the app last shaped.
	std::atomic<bool> runtime_applied{false};
	//! Bumped on every app unshape; an apply cut for an older generation is void.
	std::atomic<uint32_t> gen{0};
	//! The hook asks the worker to re-apply even if the region is unchanged.
	std::atomic<bool> force{true};
	std::atomic<uint32_t> app_calls{0};
	std::atomic<uint32_t> applied{0};
	//! 10 s INFO accumulators: worker cut time, applier SetWindowRgn time.
	uint64_t st_t0 = 0, st_cut_ns = 0, st_cut_max = 0;
	uint32_t st_cuts = 0;
	std::atomic<uint64_t> st_apply_ns{0}, st_apply_max{0};
	std::atomic<uint32_t> st_applies{0};

	// Present worker only.
	std::vector<uint8_t> hist[kHistory];
	uint32_t hist_n = 0, hist_i = 0, w = 0, h = 0, cw = 0, ch = 0;
	uint64_t last_sig = 0;
	std::vector<uint8_t> uni, dil;
	std::vector<RECT> rects;

	// Applier hand-off (latest wins).
	std::mutex m;
	std::condition_variable cv;
	bool pending = false;
	bool stop = false;
	bool done = false;
	std::vector<RECT> pend_rects;
	uint32_t pend_gen = 0;
};

// Registry + import-table hook, refcounted by the owners.
std::mutex g_reg_mutex;
std::vector<std::shared_ptr<RegionState>> g_reg;
std::vector<ULONG_PTR *> g_patched; // IAT slots we rewrote

std::shared_ptr<RegionState>
lookup(HWND h)
{
	std::lock_guard<std::mutex> lk(g_reg_mutex);
	for (auto &s : g_reg) {
		if (s->hwnd == h) {
			return s;
		}
	}
	return nullptr;
}

int WINAPI
hook_SetWindowRgn(HWND h, HRGN rgn, BOOL redraw)
{
	std::shared_ptr<RegionState> s = lookup(h);
	if (!s) {
		return g_real(h, rgn, redraw);
	}
	if (rgn != nullptr) {
		const bool first = !s->app_shaped.exchange(true);
		if (first) {
			s->force = true;
		}
		// Until the runtime has cut one from a presented frame, the app's own
		// region is the best there is — let it through so shaping starts now.
		if (!s->runtime_applied.load()) {
			return g_real(h, rgn, redraw);
		}
		s->app_calls++;
		DeleteObject(rgn); // a successful SetWindowRgn owns it; so do we
		return 1;
	}
	// Unshape: hand ownership back, synchronously, exactly as the app asked.
	s->app_shaped = false;
	s->runtime_applied = false;
	s->gen++;
	s->force = true;
	return g_real(h, nullptr, redraw);
}

void
patch_module(HMODULE mod, ULONG_PTR from, ULONG_PTR to, bool record)
{
	BYTE *base = (BYTE *)mod;
	auto *dos = (IMAGE_DOS_HEADER *)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return;
	}
	auto *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		return;
	}
	const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (dir.VirtualAddress == 0) {
		return;
	}
	for (auto *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name != 0; imp++) {
		for (auto *t = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk); t->u1.Function != 0; t++) {
			if (t->u1.Function != from) {
				continue;
			}
			DWORD old = 0;
			if (VirtualProtect(&t->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &old)) {
				t->u1.Function = to;
				VirtualProtect(&t->u1.Function, sizeof(ULONG_PTR), old, &old);
				if (record) {
					g_patched.push_back((ULONG_PTR *)&t->u1.Function);
				}
			}
		}
	}
}

//! Caller holds g_reg_mutex. Returns the number of import slots hooked.
size_t
hook_install_locked()
{
	HMODULE user32 = GetModuleHandleA("user32.dll");
	g_real = user32 != nullptr ? (PFN_SetWindowRgn)GetProcAddress(user32, "SetWindowRgn") : nullptr;
	if (g_real == nullptr) {
		return 0;
	}
	HMODULE self = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   (LPCSTR)&hook_install_locked, &self);
	HMODULE mods[1024];
	DWORD needed = 0;
	if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
		return 0;
	}
	const DWORD n = needed / sizeof(HMODULE) < 1024 ? needed / sizeof(HMODULE) : 1024;
	for (DWORD i = 0; i < n; i++) {
		if (mods[i] != self && mods[i] != user32) {
			patch_module(mods[i], (ULONG_PTR)g_real, (ULONG_PTR)&hook_SetWindowRgn, true);
		}
	}
	return g_patched.size();
}

//! Caller holds g_reg_mutex.
void
hook_remove_locked()
{
	for (ULONG_PTR *slot : g_patched) {
		DWORD old = 0;
		if (VirtualProtect(slot, sizeof(ULONG_PTR), PAGE_READWRITE, &old)) {
			*slot = (ULONG_PTR)g_real;
			VirtualProtect(slot, sizeof(ULONG_PTR), old, &old);
		}
	}
	g_patched.clear();
}

uint64_t
hash_rects(const std::vector<RECT> &r)
{
	uint64_t hsh = 0xcbf29ce484222325ull ^ r.size();
	const uint8_t *p = (const uint8_t *)r.data();
	for (size_t i = 0; i < r.size() * sizeof(RECT); i++) {
		hsh = (hsh ^ p[i]) * 0x100000001b3ull;
	}
	return hsh;
}

void
applier_main(std::shared_ptr<RegionState> s)
{
	std::vector<uint8_t> buf;
	std::unique_lock<std::mutex> lk(s->m);
	for (;;) {
		s->cv.wait(lk, [&] { return s->stop || s->pending; });
		if (s->stop) {
			break;
		}
		std::vector<RECT> rects;
		rects.swap(s->pend_rects);
		const uint32_t gen = s->pend_gen;
		s->pending = false;
		lk.unlock();

		if (s->app_shaped.load() && gen == s->gen.load()) {
			HRGN rgn = nullptr;
			if (rects.empty()) {
				rgn = CreateRectRgn(0, 0, 0, 0); // fully click-through
			} else {
				buf.resize(sizeof(RGNDATAHEADER) + rects.size() * sizeof(RECT));
				RGNDATA *rd = (RGNDATA *)buf.data();
				rd->rdh.dwSize = sizeof(RGNDATAHEADER);
				rd->rdh.iType = RDH_RECTANGLES;
				rd->rdh.nCount = (DWORD)rects.size();
				rd->rdh.nRgnSize = (DWORD)(rects.size() * sizeof(RECT));
				RECT bb = rects[0];
				for (const RECT &r : rects) {
					bb.left = r.left < bb.left ? r.left : bb.left;
					bb.top = r.top < bb.top ? r.top : bb.top;
					bb.right = r.right > bb.right ? r.right : bb.right;
					bb.bottom = r.bottom > bb.bottom ? r.bottom : bb.bottom;
				}
				rd->rdh.rcBound = bb;
				memcpy(rd->Buffer, rects.data(), rects.size() * sizeof(RECT));
				rgn = ExtCreateRegion(nullptr, (DWORD)buf.size(), rd);
			}
			const uint64_t a0 = os_monotonic_get_ns();
			const int ok = rgn != nullptr ? g_real(s->hwnd, rgn, TRUE) : 0;
			const uint64_t ad = os_monotonic_get_ns() - a0;
			s->st_apply_ns += ad;
			s->st_applies++;
			if (ad > s->st_apply_max.load()) {
				s->st_apply_max = ad;
			}
			if (ok != 0) {
				s->applied++;
				if (!s->runtime_applied.exchange(true)) {
					U_LOG_W(
					    "Runtime-owned window region: engaged on hwnd %p — the region is cut from "
					    "each "
					    "presented frame and applied right after its Present; the app's own "
					    "SetWindowRgn(non-NULL) calls are superseded, SetWindowRgn(NULL) still "
					    "unshapes "
					    "(DXR_VK_RUNTIME_REGION=0 disables)",
					    (void *)s->hwnd);
				}
				// The app unshaped while this was in flight: undo it.
				if (!s->app_shaped.load() || gen != s->gen.load()) {
					s->runtime_applied = false;
					g_real(s->hwnd, nullptr, TRUE);
				}
			} else if (rgn != nullptr) {
				DeleteObject(rgn);
			}
		}
		lk.lock();
	}
	s->done = true;
	s->cv.notify_all();
}

bool
env_disabled()
{
	const char *e = getenv("DXR_VK_RUNTIME_REGION");
	return e != nullptr && e[0] == '0';
}

} // namespace

struct comp_vk_region_owner
{
	std::shared_ptr<RegionState> s;
};

extern "C" struct comp_vk_region_owner *
comp_vk_region_owner_create(void *hwnd)
{
	if (hwnd == nullptr || env_disabled()) {
		return nullptr;
	}
	auto s = std::make_shared<RegionState>();
	s->hwnd = (HWND)hwnd;
	size_t slots = 0;
	{
		std::lock_guard<std::mutex> lk(g_reg_mutex);
		if (g_reg.empty()) {
			slots = hook_install_locked();
			if (slots == 0) {
				U_LOG_W(
				    "Runtime-owned window region: no import of user32!SetWindowRgn found to hook — the "
				    "app keeps shaping its own window");
				return nullptr;
			}
		}
		g_reg.push_back(s);
	}
	if (slots != 0) {
		U_LOG_W(
		    "Runtime-owned window region: SetWindowRgn hooked in %zu import slot(s); a window is shaped "
		    "by the runtime only after its app shapes it",
		    slots);
	}
	std::thread(applier_main, s).detach();
	auto *o = new comp_vk_region_owner();
	o->s = s;
	return o;
}

extern "C" void
comp_vk_region_owner_on_present(
    struct comp_vk_region_owner *o, const uint8_t *bgra, uint32_t w, uint32_t h, bool is_repaint)
{
	if (o == nullptr || bgra == nullptr || w == 0 || h == 0) {
		return;
	}
	RegionState &s = *o->s;
	if (!s.app_shaped.load()) {
		s.hist_n = 0;
		return;
	}
	const uint64_t t0 = os_monotonic_get_ns();
	struct CutTimer
	{
		RegionState &s;
		uint64_t t0;
		~CutTimer()
		{
			const uint64_t now = os_monotonic_get_ns(), d = now - t0;
			s.st_cut_ns += d;
			s.st_cuts++;
			s.st_cut_max = d > s.st_cut_max ? d : s.st_cut_max;
			if (s.st_t0 == 0) {
				s.st_t0 = now;
			}
			if (now - s.st_t0 >= 10ull * 1000 * 1000 * 1000) {
				const uint32_t na = s.st_applies.exchange(0);
				U_LOG_I(
				    "Runtime-owned window region: %u cut(s) %.2f ms mean (max %.2f) on the present "
				    "worker; "
				    "%u SetWindowRgn %.2f ms mean (max %.2f) on the applier; %u app call(s) superseded",
				    s.st_cuts, (double)s.st_cut_ns / s.st_cuts / 1e6, (double)s.st_cut_max / 1e6, na,
				    na ? (double)s.st_apply_ns.exchange(0) / na / 1e6 : 0.0,
				    (double)s.st_apply_max.exchange(0) / 1e6, s.app_calls.load());
				s.st_t0 = now;
				s.st_cuts = 0;
				s.st_cut_ns = 0;
				s.st_cut_max = 0;
			}
		}
	} timer{s, t0};
	if (w != s.w || h != s.h) {
		s.w = w;
		s.h = h;
		s.cw = (w + kTexel - 1) / kTexel;
		s.ch = (h + kTexel - 1) / kTexel;
		s.hist_n = 0;
		s.hist_i = 0;
		for (auto &v : s.hist) {
			v.assign((size_t)s.cw * s.ch, 0);
		}
		s.force = true;
	}
	// A repaint re-weaves the last app frame's atlas: same footprint.
	if (!is_repaint || s.hist_n == 0) {
		std::vector<uint8_t> &fp = s.hist[s.hist_i];
		std::fill(fp.begin(), fp.end(), 0);
		const uint32_t *px = (const uint32_t *)bgra;
		for (uint32_t y = 0; y < h; y++) {
			uint8_t *row = &fp[(size_t)(y / kTexel) * s.cw];
			const uint32_t *src = px + (size_t)y * w;
			for (uint32_t x = 0; x < w; x++) {
				if (src[x] & 0xFF000000u) {
					row[x / kTexel] = 1;
				}
			}
		}
		s.hist_i = (s.hist_i + 1) % kHistory;
		if (s.hist_n < kHistory) {
			s.hist_n++;
		}
	} else if (!s.force.load()) {
		return;
	}

	// Union of the last presented frames, then dilate.
	const size_t n = (size_t)s.cw * s.ch;
	s.uni.assign(n, 0);
	for (uint32_t k = 0; k < s.hist_n; k++) {
		const uint8_t *f = s.hist[k].data();
		for (size_t i = 0; i < n; i++) {
			s.uni[i] |= f[i];
		}
	}
	s.dil.assign(n, 0);
	for (uint32_t y = 0; y < s.ch; y++) {
		for (uint32_t x = 0; x < s.cw; x++) {
			if (!s.uni[(size_t)y * s.cw + x]) {
				continue;
			}
			const uint32_t y0 = y >= kDilate ? y - kDilate : 0,
			               y1 = y + kDilate < s.ch ? y + kDilate : s.ch - 1;
			const uint32_t x0 = x >= kDilate ? x - kDilate : 0,
			               x1 = x + kDilate < s.cw ? x + kDilate : s.cw - 1;
			for (uint32_t yy = y0; yy <= y1; yy++) {
				memset(&s.dil[(size_t)yy * s.cw + x0], 1, x1 - x0 + 1);
			}
		}
	}

	// One rect per horizontal run; identical consecutive rows extend the
	// previous rects instead of adding new ones.
	s.rects.clear();
	size_t prev_begin = 0, prev_count = 0;
	for (uint32_t y = 0; y < s.ch; y++) {
		const LONG top = (LONG)(y * kTexel);
		const LONG bottom = (LONG)((y + 1) * kTexel < h ? (y + 1) * kTexel : h);
		const size_t begin = s.rects.size();
		const uint8_t *row = &s.dil[(size_t)y * s.cw];
		for (uint32_t x = 0; x < s.cw;) {
			if (!row[x]) {
				x++;
				continue;
			}
			uint32_t e = x;
			while (e < s.cw && row[e]) {
				e++;
			}
			RECT r = {(LONG)(x * kTexel), top, (LONG)(e * kTexel < w ? e * kTexel : w), bottom};
			s.rects.push_back(r);
			x = e;
		}
		const size_t count = s.rects.size() - begin;
		bool same = count == prev_count && count > 0;
		for (size_t i = 0; same && i < count; i++) {
			const RECT &a = s.rects[begin + i], &b = s.rects[prev_begin + i];
			same = a.left == b.left && a.right == b.right && b.bottom == top;
		}
		if (same) {
			for (size_t i = 0; i < count; i++) {
				s.rects[prev_begin + i].bottom = bottom;
			}
			s.rects.resize(begin);
		} else {
			prev_begin = begin;
			prev_count = count;
		}
	}

	const uint64_t sig = hash_rects(s.rects);
	const bool forced = s.force.exchange(false);
	if (sig == s.last_sig && !forced) {
		return; // DWM already has exactly this region
	}
	s.last_sig = sig;
	{
		std::lock_guard<std::mutex> lk(s.m);
		s.pend_rects = s.rects;
		s.pend_gen = s.gen.load();
		s.pending = true;
	}
	s.cv.notify_one();
}

extern "C" void
comp_vk_region_owner_destroy(struct comp_vk_region_owner **op)
{
	if (op == nullptr || *op == nullptr) {
		return;
	}
	std::shared_ptr<RegionState> s = (*op)->s;
	delete *op;
	*op = nullptr;
	{
		std::lock_guard<std::mutex> lk(g_reg_mutex);
		for (size_t i = 0; i < g_reg.size(); i++) {
			if (g_reg[i] == s) {
				g_reg.erase(g_reg.begin() + i);
				break;
			}
		}
		if (g_reg.empty()) {
			hook_remove_locked();
		}
	}
	// The applier may be inside SetWindowRgn, which sends to the window's
	// thread — possibly this one. Wait briefly, never indefinitely; the
	// thread holds its own reference to the state.
	std::unique_lock<std::mutex> lk(s->m);
	s->stop = true;
	s->cv.notify_all();
	s->cv.wait_for(lk, std::chrono::milliseconds(250), [&] { return s->done; });
	U_LOG_I("Runtime-owned window region: %u region(s) applied, %u app SetWindowRgn call(s) superseded",
	        s->applied.load(), s->app_calls.load());
}
