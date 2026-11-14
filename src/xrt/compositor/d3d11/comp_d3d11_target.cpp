// Copyright 2024-2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 DXGI swapchain target implementation.
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#include "comp_d3d11_target.h"
#include "comp_d3d11_compositor.h"

#include "util/u_logging.h"
#include "util/u_debug.h"
#include "util/u_setting.h"
#include "util/u_time.h"

#include "os/os_time.h"

#include <atomic>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <dcomp.h>

// Decoupled presentation (#833): with DXR_PRESENT_OPAQUE=1 a transparent
// session presents through the opaque HWND flip-model path while the DP
// keeps compose-under-bg - the baked desktop makes the output opaque by
// construction, and the HWND flip swapchain is eligible for Hardware
// Independent Flip (DComp visuals stay at full Composed: Flip cost
// regardless of their alpha mode). See the use_transparent gate below.
DEBUG_GET_ONCE_BOOL_OPTION(present_opaque, "DXR_PRESENT_OPAQUE", false)

// #2603: the bound on a Present that runs on the bounded present worker (see
// comp_d3d11_target_set_present_bound). Unset (-1) = the caller's default,
// 0 = no worker (Present on the calling thread, as before), N = N ms.
DEBUG_GET_ONCE_NUM_OPTION(present_wedge_ms, "DXR_PRESENT_WEDGE_MS", -1)

// #2635: repaint even when the app presents through a Present hook that also
// intercepts our chain (A/B only — that combination is the deadlock).
DEBUG_GET_ONCE_BOOL_OPTION(repaint_under_present_hook, "DXR_REPAINT_UNDER_PRESENT_HOOK", false)

#include "util/comp_weave_latency_win.h"
#include "util/comp_frame_witness.h"
#include "util/comp_present_hook.h"

// One in-process D3D11 target per compositor per app process — file-scope
// harness + late-weave state (mirrors comp_d3d12_target.cpp; the target
// struct is memset-adjacent zero-init so C++ members live at file scope).
static weave_latency_log g_weave_latency_d3d11;
static comp_frame_witness g_frame_witness_d3d11{"d3d11"};

static bool
dxr_late_weave_enabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		// Default ON: late-weave is the product behavior on every path
		// (measured 96->17 ms VK, 62->17 D3D12, 32->17 D3D11, 29->17
		// workspace). DXR_LATE_WEAVE=0 opts out for A/B or triage. Read
		// through u_setting so the Dashboard's Developer settings can set
		// it for apps it never launched (perf-settings Phase 2B); the
		// environment still wins. Same parse as before.
		char lw_buf[16];
		const char *e = u_setting_get_raw("DXR_LATE_WEAVE", lw_buf, sizeof(lw_buf), nullptr);
		enabled = (e != nullptr && e[0] == '0') ? 0 : 1;
	}
	return enabled == 1;
}

// Late-weave pacing state (single target per process on this path).
static HANDLE g_frame_latency_waitable = nullptr;

/*!
 * #1339 instrumentation: how long the APP blocked on the frame-latency
 * waitable inside comp_d3d11_target_weave_mark. That wait runs on the app
 * thread WITH the compositor lock held, so it is one of the two candidate
 * causes for an app commit overrunning a whole partition stride (the other
 * being the acquisition of that lock itself).
 *
 * ATOMIC, relaxed. Raised on the app thread and TAKEN by the repaint thread
 * with an exchange, so the read and the reset are one step: nothing can raise
 * a peak between a read and a later clear and have it wiped unreported. The
 * one lock that could order this instead is the lock whose contention it
 * exists to measure, so taking it would perturb the measurement. Relaxed is
 * right — the value orders nothing else.
 *
 * Consistency note: the partition's own releases / slots_forfeited stay plain
 * uint32 read across the same two threads, as next_release_ns already was. A
 * torn read of a cumulative diagnostic is not worth an atomic; these two are
 * atomic because they are EXCHANGED, not merely read.
 */
static std::atomic<uint64_t> g_app_wait_max_ns{0};

/*!
 * #1482 instrumentation, in the same destructive-read shape as the peak above.
 *
 *  - g_app_stage2_max_ns: peak duration of STAGE 2 in weave_mark (the composed
 *    chain's compositor-clock align, or the opaque chain's scanout wait). The
 *    peak above measures stage 1 only, and the scanout wait is bounded at three
 *    panel periods — a whole stride at D=3 — under the same lock.
 *  - g_app_drained: frame-latency tokens the #868 surplus drain removed. Zero
 *    while the grid paces the app, because the drain is skipped there.
 *  - g_app_wait_timeouts: stage-1 waits that ran to the full 100 ms bound.
 *  - g_app_wait_instant: stage-1 waits that returned on an already-banked token.
 *
 * Relaxed for the same reason: they order nothing, and the one lock that could
 * order them is the lock whose cost they exist to measure.
 */
static std::atomic<uint64_t> g_app_stage2_max_ns{0};
static std::atomic<uint32_t> g_app_drained{0};
static std::atomic<uint32_t> g_app_wait_timeouts{0};
static std::atomic<uint32_t> g_app_wait_instant{0};
static UINT g_last_present_count = 0;

// Live-path pacing state (#833): the transparent/composed chain paces with the
// DComp compositor clock (its frame statistics are unreliable), the opaque
// flip chain with GetFrameStatistics — behaviorally gated: consecutive
// full-bound timeouts on a stats-succeeding chain declare the stats unusable.
static bool g_target_is_composed = false;
static int g_scanout_stats_strikes = 0; //!< consecutive TIMED_OUT; <0 = disabled
// #868 repaint presents release waitable tokens no one consumed; the app's
// next waits would return instantly for that many frames (pacing loss after
// every idle stretch). weave_mark drains the excess when this is nonzero.
static std::atomic<uint32_t> g_repaint_presents_since_app{0};
// #1482: is the app's frame release paced by a #1257 partition grid? Pushed in
// by the compositor at its throttle (@ref comp_d3d11_target_set_app_paced) —
// the grid lives on the compositor, and this target only ever sees the app
// thread arrive. One target per process on this path, like every static here.
static bool g_app_paced = false;
// #1339 repaint queue cap (see comp_d3d11_target_repaint_admit): a token taken
// by admit and not yet spent by a present. Plain statics, written on the
// repaint thread and read in comp_d3d11_target_present on BOTH threads: that
// is safe only because every present on every tier that uses this target runs
// under the compositor mutex (d3d11 c->mutex, the d3d12 reroute's lock_guard,
// the VK tier's c->mutex). An unlocked present path would need atomics here.
static bool g_repaint_holds_token = false;
/*!
 * #1571: the APP-side twin of g_repaint_holds_token. Stage 1 of weave_mark
 * takes a frame-latency token, but the pick that decides whether this frame
 * PRESENTS happens afterwards — and under the split it can legitimately come
 * back with nothing (#918 F4: the frames before anything has crossed, and the
 * frame after an egress realloc). The chain releases a token only when a
 * present RETIRES, so a token taken by a frame that then skipped its present
 * is gone for good: from that frame on every stage-1 wait runs to its full
 * 100 ms bound with c->mutex held, and comp_d3d11_target_repaint_admit refuses
 * every repaint forever, so the chain can never be re-primed either. Carrying
 * it forward is the same accounting the repaint side already does (#1339);
 * the comment on weave_mark_repaint states the failure mode in as many words.
 *
 * Plain static, like its repaint twin and for the same reason: weave_mark, the
 * F4 skip and the app's present all run on the app thread under the
 * compositor mutex. An unlocked present path would need an atomic here.
 */
static bool g_app_holds_token = false;
//! #2564: frame-latency token ledger and stage-2 gate for this chain (see
//! comp_weave_latency_win.h). One target per process on this path.
static late_weave_token_ledger g_lw_ledger_d3d11;
static late_weave_stage2_gate g_lw_stage2_d3d11;
// A repaint has been marked and its present is next on the repaint thread;
// that present (and only a present on that thread) settles the token.
static bool g_repaint_present_pending = false;
static DWORD g_repaint_thread_id = 0;
static int g_repaint_queue_cap = -1; // DXR_WEAVE_REPAINT_QUEUE_CAP, default 1

// Latency governor (#850): DXR_LATE_WEAVE_MAX_LATENCY knob + saturation
// auto-backoff (mirrors comp_d3d12_target.cpp).
static late_weave_governor g_lw_gov_d3d11;

/*
 * Present watchdog (#1000).
 *
 * The #925 S1 bound above (DXGI_PRESENT_DO_NOT_WAIT retried <=50 ms) handles a
 * FULL flip queue — DXGI returns WAS_STILL_DRAWING and we drop the frame. It
 * cannot handle the mode captured in #1000's dump: ONE Present call that never
 * returns, blocked inside the display adapter's user-mode driver on a
 * cross-adapter sync object (dGPU renders, iGPU scans out — every present on
 * this topology crosses adapters):
 *
 *   dxgi!CFlipPresentToDWM -> igd10iumd64!... ->
 *   d3d11!NDXGI::CDevice::WaitForSynchronizationObjectFromCpuCB   (minutes)
 *
 * DO_NOT_WAIT never fires there — the wait is below DXGI's queue check, during
 * submission itself. No code on the calling thread can bound it, and because
 * layer_commit holds c->mutex across the present, the repaint thread wedges
 * behind it and the process goes silent while still reporting Responding=TRUE
 * (the window thread keeps pumping). The result reads as a crash with an empty
 * log.
 *
 * So: a tiny sampling thread. The present path stamps g_present_wd_enter_ns
 * around the Present call; this thread wakes 4x/s and escalates a WARN at
 * 500 ms / 5 s / 30 s, then every 60 s, and logs recovery if the call ever
 * completes. Diagnosis only — it must never touch the swapchain.
 */
static std::atomic<int64_t> g_present_wd_enter_ns{0};
static std::atomic<uint64_t> g_present_wd_seq{0};
//! #2635: the thread inside the stuck Present (the watchdog names it).
static std::atomic<unsigned long> g_present_wd_tid{0};
static HANDLE g_present_wd_thread = nullptr;
static HANDLE g_present_wd_exit = nullptr;

static DWORD WINAPI
present_watchdog_thread_proc(LPVOID param)
{
	(void)param;
	uint64_t warned_seq = 0;
	int warned_stage = 0; // 0 none, 1 @500ms, 2 @5s, 3 @30s, 4+ repeats
	int64_t warned_enter_ns = 0;

	for (;;) {
		if (WaitForSingleObject(g_present_wd_exit, 250) == WAIT_OBJECT_0) {
			return 0;
		}

		int64_t enter_ns = g_present_wd_enter_ns.load(std::memory_order_acquire);
		uint64_t seq = g_present_wd_seq.load(std::memory_order_acquire);

		// The stuck call completed (or a new one started): report recovery once.
		if (warned_stage > 0 && (enter_ns == 0 || seq != warned_seq)) {
			int64_t stuck_ms = ((int64_t)os_monotonic_get_ns() - warned_enter_ns) / 1000000;
			U_LOG_W("d3d11 present watchdog: Present recovered after ~%lld ms (#1000)",
			        (long long)stuck_ms);
			warned_stage = 0;
		}
		if (enter_ns == 0) {
			continue;
		}

		int64_t elapsed_ms = ((int64_t)os_monotonic_get_ns() - enter_ns) / 1000000;
		if (seq != warned_seq) {
			warned_seq = seq;
			warned_stage = 0;
		}

		int stage = 0;
		if (elapsed_ms >= 30000) {
			// Stage 3 first, then one repeat WARN per further 60 s.
			stage = 3 + (int)((elapsed_ms - 30000) / 60000);
		} else if (elapsed_ms >= 5000) {
			stage = 2;
		} else if (elapsed_ms >= 500) {
			stage = 1;
		}
		if (stage > warned_stage) {
			warned_stage = stage;
			warned_enter_ns = enter_ns;
			// Two known causes, indistinguishable from here: the driver's
			// cross-adapter present submission (#1000), or a third-party
			// Present hook waiting on a lock the app thread holds (#2635).
			U_LOG_W("d3d11 present watchdog: Present has not returned for %lld ms — "
			        "blocked inside the Present call: the driver's cross-adapter present "
			        "submission (#1000), or a third-party Present hook waiting on a lock "
			        "the app's thread holds (#2635); DO_NOT_WAIT cannot bound either. "
			        "c->mutex is held, so the repaint thread is stalled behind it. "
			        "(presenting tid=%lu)",
			        (long long)elapsed_ms, g_present_wd_tid.load(std::memory_order_relaxed));
		}
	}
}


/*!
 * D3D11 target structure.
 */
struct comp_d3d11_target
{
	//! Parent compositor.
	struct comp_d3d11_compositor *c;

	//! Device/context/factory this target presents on, handed in at create.
	//! NOT owned — no AddRef/Release; the caller outlives the target.
	ID3D11Device *dev;
	ID3D11DeviceContext *ctx;
	IDXGIFactory4 *factory;

	//! DXGI swapchain.
	IDXGISwapChain1 *swapchain;

	//! Current render target view (for current back buffer).
	ID3D11RenderTargetView *rtv;

	//! Current back buffer texture.
	ID3D11Texture2D *back_buffer;

	//! Window handle.
	HWND hwnd;

	//! Current dimensions.
	uint32_t width;
	uint32_t height;

	//! Swapchain creation flags — DXGI requires ResizeBuffers to pass the
	//! exact same flags or it fails with E_INVALIDARG (#848; hit whenever
	//! the late-weave FRAME_LATENCY_WAITABLE_OBJECT chain is resized).
	UINT swapchain_flags;

	//! Current back buffer index.
	uint32_t current_index;

	//! DirectComposition resources (transparent path only — null on default path).
	//! On the transparent path the swapchain is created via
	//! CreateSwapChainForComposition (HWND-less) and bound to the app's HWND
	//! through DComp instead of via DXGI, so DWM can blend per-pixel alpha.
	IDCompositionDevice *dcomp_device;
	IDCompositionTarget *dcomp_target;
	IDCompositionVisual *dcomp_visual;

	//! The device was removed/reset under this chain (see
	//! @ref comp_d3d11_target_is_device_lost). Latched, never cleared.
	bool device_lost;
	//! Rate limit for the "still lost" WARN, and the frames it summarises.
	uint64_t lost_last_log_ns;
	uint64_t lost_frames_skipped;

	/*
	 * #2603 bounded present. When the worker runs, the Present call itself is
	 * made on pw_thread and the caller (the app or repaint thread, holding the
	 * compositor lock as before) waits for it at most present_bound_ms. The
	 * caller still blocks for exactly as long as the Present takes, so pacing
	 * and the thread-keyed token accounting below are unchanged; only a
	 * Present that never returns is cut off. 0 = no worker.
	 */
	uint32_t present_bound_ms;
	HANDLE pw_thread;
	HANDLE pw_req;  //!< auto-reset: one request
	HANDLE pw_done; //!< auto-reset: its result is in pw_hr
	HANDLE pw_exit; //!< manual-reset: worker leaves
	uint32_t pw_sync_interval;
	HRESULT pw_hr;
	//! A Present that never came back. device_lost is latched with it, and
	//! nothing may touch this chain, its device or its context again: the
	//! worker is still inside the driver, holding them. Destroy leaks them.
	bool present_wedged;

	/*
	 * #2635 present-hook re-entry. The app calls xrEndFrame from inside a
	 * third-party IDXGISwapChain::Present hook that also intercepts this
	 * chain's Present: a repaint presented from another thread while the
	 * compositor lock is held deadlocks against that hook's lock. Latched,
	 * never cleared; sampled on app-thread presents until it latches.
	 */
	bool present_reentrant;
	uint64_t reentry_next_sample_ns;
	bool reentry_inline_logged; //!< "Present moves off the worker" WARN, once
	bool present_thread_logged; //!< first-Present thread-id WARN, once
};

static bool
target_hr_is_device_loss(HRESULT hr)
{
	return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG;
}

/*!
 * A Present / ResizeBuffers failure: decide whether it means the device is
 * gone and, the first time it does, latch and log it ONCE with the removal
 * reason. Any failure is checked against GetDeviceRemovedReason, because a
 * removal can surface first as a different error code. Returns the latch.
 */
static bool
target_check_device_lost(struct comp_d3d11_target *target, HRESULT hr, const char *where)
{
	if (target->device_lost) {
		return true;
	}
	const HRESULT reason = (target->dev != nullptr) ? target->dev->GetDeviceRemovedReason() : S_OK;
	if (!target_hr_is_device_loss(hr) && reason == S_OK) {
		return false;
	}
	target->device_lost = true;
	target->lost_last_log_ns = os_monotonic_get_ns();
	target->lost_frames_skipped = 0;
	U_LOG_E(
	    "d3d11 target: the output device is LOST (%s failed: hr=0x%08x, GetDeviceRemovedReason=0x%08x). "
	    "Presenting stops and the session is reported lost; the device is not recreated in place. "
	    "Typical cause: a display hotplug or a driver reset/TDR. (logged once)",
	    where, (unsigned)hr, (unsigned)reason);
	// The chain will never retire another present, so no token is owed or
	// carried: a carry here would make every later weave_mark skip its wait.
	g_app_holds_token = false;
	g_repaint_holds_token = false;
	g_repaint_present_pending = false;
	return true;
}

/*!
 * While lost: stand in for the pacing the dead chain no longer provides, on the
 * APP thread only (the repaint thread is idle once nothing weaves, and must not
 * sleep under the compositor lock on the app's behalf). One display period,
 * the governor's measured one when known, else 60 Hz. Also emits the
 * rate-limited reminder — never a per-frame log.
 */
static void
target_lost_pace(struct comp_d3d11_target *target)
{
	target->lost_frames_skipped++;
	const uint64_t now = os_monotonic_get_ns();
	if (now - target->lost_last_log_ns >= 30ULL * 1000000000ULL) {
		target->lost_last_log_ns = now;
		U_LOG_W("d3d11 target: output device still lost — %llu frames skipped; end the session to recover",
		        (unsigned long long)target->lost_frames_skipped);
	}
	if (g_repaint_thread_id != 0 && GetCurrentThreadId() == g_repaint_thread_id) {
		return;
	}
	g_app_holds_token = false;
	const double p = g_lw_gov_d3d11.period_ns;
	const int64_t period_ns = (p > 1e6 && p < 1e9) ? (int64_t)p : (int64_t)(1000000000LL / 60);
	os_nanosleep(period_ns);
}

extern "C" bool
comp_d3d11_target_is_device_lost(struct comp_d3d11_target *target)
{
	return target != nullptr && target->device_lost;
}

extern "C" bool
comp_d3d11_target_is_present_wedged(struct comp_d3d11_target *target)
{
	return target != nullptr && target->present_wedged;
}

/*
 * #2635 — present-hook re-entry probe (see util/comp_present_hook.h).
 *
 * Read-only: VirtualQuery before every read of foreign code, module lookups
 * without taking a reference, and nothing is ever written or called.
 */
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
reentry_present_hook_owner(IDXGISwapChain1 *swapchain)
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

/*!
 * Sample on an app-thread present (not the repaint thread): rate-limited to
 * once a second, and stops once latched. Microseconds when it runs.
 */
static void
target_reentry_sample(struct comp_d3d11_target *target)
{
	if (target->present_reentrant) {
		return;
	}
	const uint64_t now = os_monotonic_get_ns();
	if (now < target->reentry_next_sample_ns) {
		return;
	}
	// An unhooked chain is re-checked once a second (a hook installed later
	// is rare). A hooked chain is checked on EVERY app present until it
	// latches: one present handed to another thread while the app sits in the
	// hook is enough to deadlock, so the check must never lag a present.
	const HMODULE owner = reentry_present_hook_owner(target->swapchain);
	struct comp_present_hook_range range = {};
	if (owner == nullptr || !reentry_module_range(owner, &range)) {
		target->reentry_next_sample_ns = now + U_TIME_1S_IN_NS;
		return;
	}
	void *raw[62] = {};
	const USHORT n = RtlCaptureStackBackTrace(0, 62, raw, nullptr);
	uint64_t frames[62] = {};
	for (USHORT i = 0; i < n; i++) {
		frames[i] = reinterpret_cast<uint64_t>(raw[i]);
	}
	if (!comp_present_hook_stack_hits(frames, n, &range, 1)) {
		return;
	}
	target->present_reentrant = true;
	wchar_t wpath[MAX_PATH] = {};
	char name[MAX_PATH] = "?";
	if (GetModuleFileNameW(owner, wpath, MAX_PATH) != 0) {
		const wchar_t *base = wcsrchr(wpath, L'\\');
		WideCharToMultiByte(CP_UTF8, 0, base != nullptr ? base + 1 : wpath, -1, name, (int)sizeof(name),
		                    nullptr, nullptr);
	}
	const bool overridden = debug_get_bool_option_repaint_under_present_hook();
	U_LOG_W("d3d11 target: the app calls xrEndFrame from inside a Present hook (%s) that also intercepts "
	        "this chain's Present (app tid=%lu) — a Present of this chain from any other thread would "
	        "deadlock against the hook's lock (#2635). Every Present now runs inline on the app's thread "
	        "inside xrEndFrame (the #2603 bounded-present worker is bypassed for this session). %s",
	        name, (unsigned long)GetCurrentThreadId(),
	        overridden ? "Repaints stay ON (DXR_REPAINT_UNDER_PRESENT_HOOK=1)."
	                   : "Repaints are OFF for this session (DXR_REPAINT_UNDER_PRESENT_HOOK=1 overrides).");
}

//! The thread that will make this target's Present call: the #2603 worker,
//! or the caller when the present runs inline.
static DWORD
target_presenting_tid(struct comp_d3d11_target *target, bool inline_present)
{
	if (!inline_present && target->pw_thread != nullptr) {
		return GetThreadId(target->pw_thread);
	}
	return GetCurrentThreadId();
}

extern "C" bool
comp_d3d11_target_repaint_forbidden(struct comp_d3d11_target *target)
{
	return target != nullptr && target->present_reentrant && !debug_get_bool_option_repaint_under_present_hook();
}

/*!
 * The Present call itself: the #925 S1 bounded DO_NOT_WAIT loop, stamped for
 * the #1000 watchdog. Runs on the calling thread, or on the #2603 worker.
 */
static HRESULT
target_present_call(struct comp_d3d11_target *target, uint32_t sync_interval)
{
	HRESULT hr;
	// Present watchdog (#1000): stamp entry so the sampling thread can name a
	// Present call that never returns (in-driver cross-adapter wait — the
	// DO_NOT_WAIT bound below never sees it). seq before enter: the watchdog
	// keys re-warn state on seq, and a stale seq with a fresh enter would
	// suppress the first warning of a new incident.
	g_present_wd_seq.fetch_add(1, std::memory_order_relaxed);
	g_present_wd_tid.store((unsigned long)GetCurrentThreadId(), std::memory_order_relaxed);
	g_present_wd_enter_ns.store((int64_t)os_monotonic_get_ns(), std::memory_order_release);
	if (sync_interval == 0) {
		hr = target->swapchain->Present(0, 0); // immediate — never blocks on the chain
	} else {
		int64_t deadline_ns = (int64_t)os_monotonic_get_ns() + 50LL * 1000000LL;
		for (;;) {
			hr = target->swapchain->Present(sync_interval, DXGI_PRESENT_DO_NOT_WAIT);
			if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
				break;
			}
			if ((int64_t)os_monotonic_get_ns() >= deadline_ns) {
				static std::atomic<int64_t> s_last_drop_log_ns{0};
				int64_t now_ns = (int64_t)os_monotonic_get_ns();
				int64_t prev_ns = s_last_drop_log_ns.load(std::memory_order_relaxed);
				if (now_ns - prev_ns > 1000000000LL &&
				    s_last_drop_log_ns.compare_exchange_strong(prev_ns, now_ns)) {
					U_LOG_W("d3d11 target present: frame dropped after 50 ms — "
					        "DWM not consuming flips (#924/#925)");
				}
				break;
			}
			Sleep(1);
		}
	}
	g_present_wd_enter_ns.store(0, std::memory_order_release);
	return hr;
}

/*
 * #2603 — the bounded present worker.
 *
 * After a GPU timeout/reset (TDR) on a two-GPU box, the hub's Present on the
 * scanout adapter's chain can block forever inside the driver: Suki's full
 * dumps show the app thread in igd10umt64xe's condition-variable wait under
 * dxgi!CDXGISwapChain::Present, with every driver worker idle — the wake-up
 * was lost in the reset, so the call never returns. #2588's latch runs only
 * when Present RETURNS an error, and the #1000 watchdog can only log; the app
 * thread is stuck inside xrEndFrame holding the compositor lock, so the app
 * hangs ("not responding") instead of seeing its session lost.
 *
 * So the call moves to a worker and the caller waits for it with a bound.
 * Healthy, the caller blocks for exactly the Present's own duration (one
 * request, one done; the pair of events is the whole hand-off), so the pacing
 * this Present provides and every caller-thread-keyed rule below are as
 * before. Past the bound the target latches LOST and WEDGED, the caller
 * returns, xrWaitFrame reports the session lost (#2588's channel), and the
 * worker is abandoned inside the driver. Nothing may touch the chain, its
 * device or its context again: the worker still holds them, and with the
 * weaver's multithread protection on, any immediate-context call would block
 * behind it. Destroy therefore leaks them instead of releasing them.
 */
static DWORD WINAPI
present_worker_proc(LPVOID param)
{
	auto *target = static_cast<comp_d3d11_target *>(param);
	HANDLE waits[2] = {target->pw_exit, target->pw_req};
	for (;;) {
		const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
		if (w != WAIT_OBJECT_0 + 1) {
			return 0;
		}
		target->pw_hr = target_present_call(target, target->pw_sync_interval);
		SetEvent(target->pw_done);
	}
}

static void
target_close_present_worker_handles(struct comp_d3d11_target *target)
{
	HANDLE *hs[] = {&target->pw_thread, &target->pw_req, &target->pw_done, &target->pw_exit};
	for (HANDLE *h : hs) {
		if (*h != nullptr) {
			CloseHandle(*h);
			*h = nullptr;
		}
	}
}

extern "C" void
comp_d3d11_target_set_present_bound(struct comp_d3d11_target *target, uint32_t default_ms)
{
	if (target == nullptr || target->pw_thread != nullptr) {
		return;
	}
	const int64_t env_ms = debug_get_num_option_present_wedge_ms();
	const uint32_t ms = env_ms >= 0 ? (uint32_t)env_ms : default_ms;
	if (ms == 0) {
		if (env_ms == 0 && default_ms != 0) {
			U_LOG_W("d3d11 target: bounded present OFF (DXR_PRESENT_WEDGE_MS=0) — a Present that never "
			        "returns hangs the app (#2603)");
		}
		return;
	}
	target->pw_req = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	target->pw_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	target->pw_exit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (target->pw_req != nullptr && target->pw_done != nullptr && target->pw_exit != nullptr) {
		target->pw_thread = CreateThread(nullptr, 0, present_worker_proc, target, 0, nullptr);
	}
	if (target->pw_thread == nullptr) {
		target_close_present_worker_handles(target);
		U_LOG_W("d3d11 target: bounded present worker failed to start — Present stays on the calling "
		        "thread (#2603)");
		return;
	}
	target->present_bound_ms = ms;
	U_LOG_W("d3d11 target: Present runs on a bounded worker (%u ms%s) — a Present that never returns "
	        "ends the session instead of hanging the app (#2603)",
	        ms, env_ms >= 0 ? ", DXR_PRESENT_WEDGE_MS" : "");
}

/*!
 * The worker's Present did not come back within the bound: latch LOST and
 * WEDGED, once. No call into the device here — not even
 * GetDeviceRemovedReason: the worker is inside the driver with the device.
 */
static void
target_mark_present_wedged(struct comp_d3d11_target *target)
{
	target->present_wedged = true;
	if (!target->device_lost) {
		target->device_lost = true;
		target->lost_last_log_ns = os_monotonic_get_ns();
		target->lost_frames_skipped = 0;
	}
	U_LOG_E("d3d11 target: Present has not returned after %u ms — the output device is treated as LOST "
	        "(#2603). Typical cause: a GPU timeout/reset (TDR) the driver never wakes the present from. "
	        "The presenting thread is abandoned and the session is reported lost; the chain, its device "
	        "and context are left untouched so teardown cannot block on them. (logged once)",
	        target->present_bound_ms);
	// As in target_check_device_lost: the chain retires nothing more, so no
	// token is owed or carried.
	g_app_holds_token = false;
	g_repaint_holds_token = false;
	g_repaint_present_pending = false;
}

static xrt_result_t
create_rtv(struct comp_d3d11_target *target)
{
	// Release existing RTV and back buffer
	if (target->rtv != nullptr) {
		target->rtv->Release();
		target->rtv = nullptr;
	}
	if (target->back_buffer != nullptr) {
		target->back_buffer->Release();
		target->back_buffer = nullptr;
	}

	// Get back buffer
	HRESULT hr = target->swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
	                                           reinterpret_cast<void **>(&target->back_buffer));
	if (FAILED(hr)) {
		U_LOG_E("Failed to get back buffer: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Create RTV
	hr = target->dev->CreateRenderTargetView(target->back_buffer, nullptr, &target->rtv);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create RTV: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d11_target_create(struct comp_d3d11_compositor *c,
                         void *hwnd,
                         void *device,
                         void *context,
                         void *dxgi_factory,
                         uint32_t width,
                         uint32_t height,
                         bool transparent,
                         struct comp_d3d11_target **out_target)
{
	comp_d3d11_target *target = new comp_d3d11_target();
	target->c = c;
	// Borrowed, not owned — the caller keeps the references.
	target->dev = static_cast<ID3D11Device *>(device);
	target->ctx = static_cast<ID3D11DeviceContext *>(context);
	target->factory = static_cast<IDXGIFactory4 *>(dxgi_factory);
	target->hwnd = static_cast<HWND>(hwnd);
	target->width = width;
	target->height = height;
	target->current_index = 0;
	target->dcomp_device = nullptr;
	target->dcomp_target = nullptr;
	target->dcomp_visual = nullptr;

	// Create swapchain.
	//
	// Default: flip-model + ALPHA_MODE_IGNORE (#163) — opaque present, no DWM bleed-through.
	// Transparent opt-in (only when an HWND was provided): flip-model +
	// ALPHA_MODE_PREMULTIPLIED via CreateSwapChainForComposition, bound to the app's
	// HWND through DirectComposition (IDCompositionTarget::SetRoot(visual)).
	// DComp gives us per-pixel alpha — no chroma-key, no disocclusion fringes, no
	// LWA_COLORKEY on the plugin side.
	// Decoupled presentation (#833): DXR_PRESENT_OPAQUE=1 keeps the DP's
	// compose-under-bg (the compositor still passes transparent=true to the
	// DP) but presents through the opaque HWND flip-model path — the baked
	// desktop makes the output opaque by construction, and an HWND flip
	// swapchain on the app's WS_EX_NOREDIRECTIONBITMAP window is eligible for
	// Hardware Independent Flip (dwm ~1 % instead of ~32 % measured on Intel
	// iGPUs; DComp visuals stay at full Composed: Flip cost regardless of
	// their alpha mode).
	const bool present_opaque = debug_get_bool_option_present_opaque();
	const bool use_transparent = transparent && hwnd != nullptr && !present_opaque;
	g_target_is_composed = use_transparent;
	g_scanout_stats_strikes = 0;
	if (transparent && hwnd != nullptr && present_opaque) {
		U_LOG_W("DXR_PRESENT_OPAQUE: HWND flip-model opaque present; DP compose-under-bg keeps "
		        "the transparent look (#833)");
	}

	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 2;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	if (use_transparent) {
		desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
	} else {
		// IGNORE so DWM doesn't composite the desktop through the bound HWND (#163).
		desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	}
	desc.Flags = 0;
	// Live-path late-weave: the waitable now goes on the COMPOSITION swapchain
	// too — live+shaped is the product default and it must be paceable. On the
	// composed chain the waitable releases at DWM *pickup* (≈1 interval before
	// scanout); the scanout-proximate second stage there is the DComp
	// compositor clock (see weave_mark), since GetFrameStatistics is
	// unreliable on composition swapchains.
	if (dxr_late_weave_enabled()) {
		desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
		// A waitable chain needs one buffer more than the deepest governor
		// level it must realize. With bc=2 any depth above 1 is a lie: the
		// waitable hands out a second token, the second Present finds no free
		// buffer, and the #925 bound below turns Present(DO_NOT_WAIT) into a
		// Present + Sleep(1) retry loop for a whole period on EVERY frame —
		// ~85 % of a core on the app's main thread and R one frame late,
		// until the governor's probe dwell (30 s) returns it to depth 1
		// (#2402: 3 of 33 single-GPU runs, latched by start-up jitter on the
		// first slip window). The composition chain carried this fix since
		// the live-path late-weave landed; the opaque HWND chain did not.
		desc.BufferCount = 3;
	}
	target->swapchain_flags = desc.Flags;

	HRESULT hr;
	if (use_transparent) {
		hr = target->factory->CreateSwapChainForComposition(target->dev, &desc, nullptr, &target->swapchain);
		U_LOG_W("Transparent HWND opt-in: DComp + flip-model swapchain "
		        "(FLIP_DISCARD + PREMULTIPLIED, bc=%u)",
		        desc.BufferCount);
	} else {
		DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc = {};
		fsDesc.Windowed = TRUE;
		// #2564: DXGI_SCALING_NONE, not the default STRETCH. During a resize
		// drag DWM keeps showing the last presented buffer until a frame of
		// the new size arrives, and STRETCH rescales it to the new window,
		// which rescales the interlace pattern: the 3D breaks up on every
		// drag step, at a steady 60 fps and constant weave timing. NONE shows
		// the stale frame 1:1 from the top-left, still correctly woven.
		// Found on Leia SR's own D3D12 sample (LeiaSR#854) and A/B'd on the
		// panel. HWND chains only: CreateSwapChainForComposition requires
		// STRETCH.
		desc.Scaling = DXGI_SCALING_NONE;
		hr = target->factory->CreateSwapChainForHwnd(target->dev, target->hwnd, &desc, &fsDesc, nullptr,
		                                             &target->swapchain);
	}
	if (FAILED(hr)) {
		U_LOG_E("Failed to create swapchain: 0x%08x", hr);
		delete target;
		return XRT_ERROR_D3D;
	}

	if ((desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) {
		IDXGISwapChain2 *sc2 = nullptr;
		if (SUCCEEDED(target->swapchain->QueryInterface(__uuidof(IDXGISwapChain2),
		                                                reinterpret_cast<void **>(&sc2)))) {
			const int lat = g_lw_gov_d3d11.base_latency();
			// #2564: a fresh chain owes no tokens and has no presentation mode yet.
			g_lw_ledger_d3d11.reset();
			g_lw_stage2_d3d11.reset();
			g_lw_ledger_d3d11.apply(sc2, lat);
			g_frame_latency_waitable = sc2->GetFrameLatencyWaitableObject();
			// #1571: a fresh semaphore owes nobody anything. These statics
			// outlive a session (one target per process on this path), so a
			// carry left over from the previous chain would make this
			// chain's first frame skip a wait it has not paid for.
			g_app_holds_token = false;
			g_repaint_holds_token = false;
			sc2->Release();
			U_LOG_W("Late-weave: D3D11 in-process swapchain waitable, max latency %d%s (waitable=%p)",
			        lat,
			        (lat == 1 && g_lw_gov_d3d11.auto_backoff == 1) ? " + saturation auto-backoff"
			                                                       : "",
			        g_frame_latency_waitable);
		}
	}

	// Disable Alt-Enter fullscreen toggle (HWND-bound only — composition swapchains
	// have no HWND association).
	if (!use_transparent) {
		target->factory->MakeWindowAssociation(target->hwnd, DXGI_MWA_NO_ALT_ENTER);
	}

	// Transparent path: bind the composition swapchain to the HWND through DComp.
	if (use_transparent) {
		hr = DCompositionCreateDevice2(
		    /*renderingDevice*/ nullptr,
		    __uuidof(IDCompositionDevice),
		    reinterpret_cast<void **>(&target->dcomp_device));
		if (FAILED(hr) || target->dcomp_device == nullptr) {
			U_LOG_E("DCompositionCreateDevice2 failed: 0x%08x", hr);
			target->swapchain->Release();
			delete target;
			return XRT_ERROR_D3D;
		}

		hr = target->dcomp_device->CreateTargetForHwnd(
		    target->hwnd, /*topmost*/ TRUE, &target->dcomp_target);
		if (FAILED(hr) || target->dcomp_target == nullptr) {
			U_LOG_E("IDCompositionDevice::CreateTargetForHwnd failed: 0x%08x", hr);
			target->dcomp_device->Release();
			target->swapchain->Release();
			delete target;
			return XRT_ERROR_D3D;
		}

		hr = target->dcomp_device->CreateVisual(&target->dcomp_visual);
		if (FAILED(hr) || target->dcomp_visual == nullptr) {
			U_LOG_E("IDCompositionDevice::CreateVisual failed: 0x%08x", hr);
			target->dcomp_target->Release();
			target->dcomp_device->Release();
			target->swapchain->Release();
			delete target;
			return XRT_ERROR_D3D;
		}

		hr = target->dcomp_visual->SetContent(target->swapchain);
		if (SUCCEEDED(hr)) {
			hr = target->dcomp_target->SetRoot(target->dcomp_visual);
		}
		if (SUCCEEDED(hr)) {
			hr = target->dcomp_device->Commit();
		}
		if (FAILED(hr)) {
			U_LOG_E("DComp visual setup failed: 0x%08x", hr);
			target->dcomp_visual->Release();
			target->dcomp_target->Release();
			target->dcomp_device->Release();
			target->swapchain->Release();
			delete target;
			return XRT_ERROR_D3D;
		}
	}

	// Create initial RTV
	xrt_result_t xret = create_rtv(target);
	if (xret != XRT_SUCCESS) {
		if (target->dcomp_visual != nullptr) target->dcomp_visual->Release();
		if (target->dcomp_target != nullptr) target->dcomp_target->Release();
		if (target->dcomp_device != nullptr) target->dcomp_device->Release();
		target->swapchain->Release();
		delete target;
		return xret;
	}

	// Present watchdog (#1000) — see the file-scope block above. Optional:
	// failure to start it must not fail target creation.
	if (g_present_wd_thread == nullptr) {
		g_present_wd_enter_ns.store(0, std::memory_order_relaxed);
		g_present_wd_exit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (g_present_wd_exit != nullptr) {
			g_present_wd_thread =
			    CreateThread(nullptr, 0, present_watchdog_thread_proc, nullptr, 0, nullptr);
			if (g_present_wd_thread == nullptr) {
				CloseHandle(g_present_wd_exit);
				g_present_wd_exit = nullptr;
				U_LOG_W("d3d11 target: present watchdog thread failed to start (#1000)");
			}
		}
	}

	*out_target = target;

	U_LOG_I("Created D3D11 target: %ux%u", width, height);

	return XRT_SUCCESS;
}

extern "C" void
comp_d3d11_target_destroy(struct comp_d3d11_target **target_ptr)
{
	if (target_ptr == nullptr || *target_ptr == nullptr) {
		return;
	}

	comp_d3d11_target *target = *target_ptr;

	// #1456: the join-coverage verdict belongs to this chain; the file-scope
	// log outlives it and serves the next session in this process.
	g_weave_latency_d3d11.po_gate_reset();
	// #1339: so does the repaint token state — the waitable it was taken
	// from dies with the chain (the repaint thread is already joined).
	g_repaint_holds_token = false;
	g_repaint_present_pending = false;
	g_repaint_thread_id = 0;
	g_repaint_presents_since_app.store(0, std::memory_order_relaxed);
	// #1482: and the grid flag — the next session republishes it at its first
	// throttle, and until then "not paced" is the reading that keeps #868's
	// drain armed.
	g_app_paced = false;

	// Stop the present watchdog (#1000). Join with a bound: if the watchdog is
	// somehow wedged we must not hang teardown for it.
	if (g_present_wd_thread != nullptr) {
		SetEvent(g_present_wd_exit);
		WaitForSingleObject(g_present_wd_thread, 2000);
		CloseHandle(g_present_wd_thread);
		CloseHandle(g_present_wd_exit);
		g_present_wd_thread = nullptr;
		g_present_wd_exit = nullptr;
		g_present_wd_enter_ns.store(0, std::memory_order_relaxed);
	}

	if (g_frame_latency_waitable != nullptr) {
		CloseHandle(g_frame_latency_waitable);
		g_frame_latency_waitable = nullptr;
		g_last_present_count = 0;
		// #1571: the semaphore is gone; no token is outstanding against it.
		g_app_holds_token = false;
		g_repaint_holds_token = false;
		g_lw_ledger_d3d11.reset();
		g_lw_stage2_d3d11.reset();
	}

	if (target->present_wedged) {
		// #2603: the worker is still inside the driver's Present on this
		// chain. Releasing the chain (or anything bound to its device) would
		// block behind it — or free what it is using. Leave the chain, its
		// buffers, the worker and the target itself allocated; the process
		// is expected to end its session and exit.
		U_LOG_W("d3d11 target: leaving the wedged chain and its present worker unreleased (#2603)");
		*target_ptr = nullptr;
		return;
	}
	if (target->pw_thread != nullptr) {
		SetEvent(target->pw_exit);
		if (WaitForSingleObject(target->pw_thread, 2000) != WAIT_OBJECT_0) {
			// A worker that cannot leave is mid-Present: same as wedged.
			U_LOG_W("d3d11 target: present worker did not exit — leaving the chain unreleased (#2603)");
			*target_ptr = nullptr;
			return;
		}
		target_close_present_worker_handles(target);
	}

	if (target->rtv != nullptr) {
		target->rtv->Release();
	}
	if (target->back_buffer != nullptr) {
		target->back_buffer->Release();
	}

	// Release DComp resources before the swapchain (visual holds a swapchain reference;
	// target holds the visual). DWM tears down the on-screen content when target releases.
	if (target->dcomp_visual != nullptr) {
		target->dcomp_visual->Release();
	}
	if (target->dcomp_target != nullptr) {
		target->dcomp_target->Release();
	}
	if (target->dcomp_device != nullptr) {
		target->dcomp_device->Release();
	}

	if (target->swapchain != nullptr) {
		target->swapchain->Release();
	}

	delete target;
	*target_ptr = nullptr;
}

extern "C" xrt_result_t
comp_d3d11_target_acquire(struct comp_d3d11_target *target, uint32_t *out_index)
{
	// For FLIP_DISCARD swapchain, we always render to buffer 0
	// The swapchain handles double-buffering internally
	*out_index = 0;
	target->current_index = 0;

	// Lost device: fail fast without touching it, and pace the app thread
	// so a frame loop over the dead chain cannot spin.
	if (target->device_lost) {
		target_lost_pace(target);
		return XRT_ERROR_D3D;
	}
	// A failed resize released the back buffer and RTV; there is nothing to
	// clear or bind until a resize succeeds.
	if (target->rtv == nullptr) {
		return XRT_ERROR_D3D;
	}

	// Bind the render target
	target->ctx->OMSetRenderTargets(1, &target->rtv, nullptr);

	// Set viewport
	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = static_cast<float>(target->width);
	viewport.Height = static_cast<float>(target->height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	target->ctx->RSSetViewports(1, &viewport);

	// Clear to black
	float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	target->ctx->ClearRenderTargetView(target->rtv, clear_color);

	return XRT_SUCCESS;
}

extern "C" void
comp_d3d11_target_bind(struct comp_d3d11_target *target)
{
	// Re-bind the render target and viewport (no clear)
	target->ctx->OMSetRenderTargets(1, &target->rtv, nullptr);

	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = static_cast<float>(target->width);
	viewport.Height = static_cast<float>(target->height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	target->ctx->RSSetViewports(1, &viewport);
}

/*!
 * #868: pace a repaint to the panel — the wait half of weave_mark, split out so
 * it can run WITHOUT the compositor lock held.
 *
 * Deliberately does NOT wait on the frame-latency waitable: that is a SEMAPHORE,
 * and a non-presenting thread waiting on it consumes tokens the app's own frame
 * loop needs (measured on the D3D12 leg: 31.9 -> 16.9 fps with essentially zero
 * repaints issued). Scanout pacing alone is passive and costs nothing.
 */
static void
d3d11_target_repaint_pace_on(IDXGISwapChain1 *swapchain);

extern "C" void
comp_d3d11_target_repaint_pace(struct comp_d3d11_target *target)
{
	d3d11_target_repaint_pace_on(target->swapchain);
}

extern "C" void *
comp_d3d11_target_repaint_pace_hold(struct comp_d3d11_target *target)
{
	if (target == nullptr || target->swapchain == nullptr) {
		return nullptr;
	}
	target->swapchain->AddRef();
	return target->swapchain;
}

extern "C" void
comp_d3d11_target_repaint_pace_held(void *hold)
{
	if (hold == nullptr) {
		return;
	}
	auto *sc = static_cast<IDXGISwapChain1 *>(hold);
	d3d11_target_repaint_pace_on(sc);
	sc->Release();
}

static void
d3d11_target_repaint_pace_on(IDXGISwapChain1 *swapchain)
{
	// Composed chains: statistics are unusable and measured_r is 0, so BOTH
	// pacers below no-op — leaving Present(1) as the only throttle, blocking
	// inside c->mutex (which this function exists to avoid). Stats-free QPC
	// deadline instead: at most one repaint per period, paced out here, then
	// a compositor-clock alignment so the repaint weave gets the same
	// constant phase the app path gets.
	if (g_target_is_composed) {
		static uint64_t s_last_ns = 0;
		const uint64_t now_ns = os_monotonic_get_ns();
		const double period_ns = (g_lw_gov_d3d11.period_ns > 0.0) ? g_lw_gov_d3d11.period_ns : 16.7e6;
		if (s_last_ns != 0 && now_ns > s_last_ns) {
			const int64_t remain = (int64_t)(s_last_ns + (uint64_t)period_ns) - (int64_t)now_ns;
			if (remain > 0 && remain < (int64_t)(period_ns * 2.0)) {
				os_nanosleep(remain);
			}
		}
		s_last_ns = os_monotonic_get_ns();
		late_weave_wait_compositor_clock(period_ns);
		return;
	}

	if (g_frame_latency_waitable != nullptr && g_scanout_stats_strikes >= 0) {
		const UINT relax = (UINT)(g_lw_gov_d3d11.effective - 1);
		if (g_last_present_count > relax) {
			late_weave_wait_scanout(swapchain, g_last_present_count - relax, g_lw_gov_d3d11.period_ns,
			                        g_weave_latency_d3d11.freq());
		}
	}

	/*
	 * #884: LATE-weave, not early-weave (ported from comp_vk_native_target).
	 *
	 * The scanout wait above returns just after the PREVIOUS frame hit glass
	 * — the START of a refresh period. Weaving there samples the eyes a whole
	 * period before the pixels are scanned out, which is exactly the staleness
	 * the repaint exists to remove. The next scanout is one period away and
	 * measured_r_ns is how long a weave takes to reach glass, so the last safe
	 * moment to begin is period - R.
	 *
	 * Clamped hard: a quarter-period floor is always kept in hand, and with no
	 * measurement yet (R = 0) we weave immediately rather than guess. A repaint
	 * that arrives late is a dropped repaint; an early one is merely stale —
	 * never gamble the frame.
	 */
	const double period_ns = g_lw_gov_d3d11.period_ns;
	const double residual_ns = (double)g_weave_latency_d3d11.measured_r_ns;
	if (period_ns <= 0.0 || residual_ns <= 0.0) {
		return; // unmeasured — weave now rather than guess
	}
	double slack_ns = period_ns - residual_ns;
	const double floor_ns = period_ns * 0.25; // never cut it finer than this
	if (slack_ns > period_ns - floor_ns) {
		slack_ns = period_ns - floor_ns;
	}
	if (slack_ns <= 0.0) {
		return; // the weave already fills the period; go now
	}
	static bool logged = false;
	if (!logged) {
		logged = true;
		U_LOG_W("#884: repaint late-weave pacing engaged (period %.1f ms, weave residual %.1f ms, "
		        "sleeping %.1f ms)",
		        period_ns / 1e6, residual_ns / 1e6, slack_ns / 1e6);
	}
	os_nanosleep((int64_t)slack_ns);
}

extern "C" bool
comp_d3d11_target_repaint_admit(struct comp_d3d11_target *target)
{
	if (target != nullptr && target->device_lost) {
		return false; // nothing can present on a dead chain
	}
	if (comp_d3d11_target_repaint_forbidden(target)) {
		return false; // #2635: a repaint present would deadlock against the app's Present hook
	}
	if (g_repaint_queue_cap < 0) {
		const char *e = getenv("DXR_WEAVE_REPAINT_QUEUE_CAP");
		g_repaint_queue_cap = (e != nullptr && e[0] == '0') ? 0 : 1;
	}
	if (g_repaint_queue_cap == 0 || g_frame_latency_waitable == nullptr) {
		return true;
	}
	if (g_repaint_holds_token) {
		return true; // taken on an earlier tick that then bailed; still ours
	}
	if (WaitForSingleObjectEx(g_frame_latency_waitable, 0, FALSE) != WAIT_OBJECT_0) {
		g_weave_latency_d3d11.note_repaint_refused();
		static bool logged = false;
		if (!logged) {
			logged = true;
			U_LOG_W("#1339: repaint refused — the present queue is at the frame-latency cap, and a "
			        "repaint queued behind a pending present is latency, not a fill (logged once; the "
			        "horizon trace row counts them; DXR_WEAVE_REPAINT_QUEUE_CAP=0 restores the old "
			        "behaviour)");
		}
		return false;
	}
	g_repaint_holds_token = true;
	return true;
}

/*!
 * #868: stamp T_weave for a repaint and nothing else — no governor EMA (repaints
 * land a panel period apart by construction and would collapse the saturation
 * signal) and no predicted display time (xrWaitFrame promised no photon time for
 * this present, so there is nothing to score it against).
 */
extern "C" void
comp_d3d11_target_weave_mark_repaint(struct comp_d3d11_target *target, bool mode_3d)
{
	(void)target;
	g_frame_witness_d3d11.count_weave(true, mode_3d);
	g_weave_latency_d3d11.mark_weave("d3d11", 0, true);
	// #1339: the token accounting settles at the PRESENT, not here — a repaint
	// that bails between mark and present (#918 F7 slot-not-ready, the race
	// re-tests) or whose present is dropped must keep its token for the next
	// tick; spending it here would consume a token the chain never releases,
	// and one such bail would pin the app's waitable wait at its 100 ms
	// timeout for the life of the chain.
	g_repaint_present_pending = true;
	g_repaint_thread_id = GetCurrentThreadId();
	// #1339: tell the governor repaints are flowing (see late_weave_governor).
	LARGE_INTEGER q;
	QueryPerformanceCounter(&q);
	g_lw_gov_d3d11.note_repaint((uint64_t)q.QuadPart);
}

extern "C" uint64_t
comp_d3d11_target_app_wait_take_max_ns(void)
{
	return g_app_wait_max_ns.exchange(0, std::memory_order_relaxed);
}

extern "C" uint64_t
comp_d3d11_target_app_stage2_take_max_ns(void)
{
	return g_app_stage2_max_ns.exchange(0, std::memory_order_relaxed);
}

extern "C" uint32_t
comp_d3d11_target_app_take_drained(void)
{
	return g_app_drained.exchange(0, std::memory_order_relaxed);
}

extern "C" uint32_t
comp_d3d11_target_app_take_wait_timeouts(void)
{
	return g_app_wait_timeouts.exchange(0, std::memory_order_relaxed);
}

extern "C" uint32_t
comp_d3d11_target_app_take_wait_instant(void)
{
	return g_app_wait_instant.exchange(0, std::memory_order_relaxed);
}


extern "C" void
comp_d3d11_target_weave_mark(struct comp_d3d11_target *target, uint64_t predicted_display_time_ns, bool mode_3d)
{
	g_frame_witness_d3d11.count_weave(false, mode_3d);
	// Late-weave pacing, two stages. Stage 1: the waitable caps the queue.
	// Stage 2 is chain-dependent — opaque flip chains wait on frame
	// statistics until the previous present actually reaches glass (DWM
	// pickup releases the waitable 2-3 frames early); composed chains have
	// unreliable statistics, so they align to the DComp compositor clock
	// instead. Bounded everywhere so occluded windows never wedge.
	bool wait_timed_out = false;
	if (g_frame_latency_waitable != nullptr) {
		// #868 interplay: a repaint present that held no token released one
		// nobody consumed. Left alone, the wait below returns instantly for
		// that many frames — pacing loss after every idle stretch. Drain the
		// surplus first; the blocking wait then re-syncs to the real present
		// cadence.
		//
		// #1482: NOT while the grid paces the app. The throttle in wait_frame
		// already released this frame on its slot, so there is no pacing left
		// for the drain to protect, and an instant stage-1 return is exactly
		// what the slot wants. What the drain does there is self-inflicted:
		// tokens are fungible, so emptying the semaphore also takes the APP's
		// own credit — and the app then blocks below on a signal it just
		// deleted, holding c->mutex. The repaint thread, the only other
		// presenter, bails on app_frame_in_progress before that lock and again
		// under it, so with no present still in flight to retire nothing can
		// refill the semaphore and the wait runs to its full bound.
		//
		// The counter is exchanged to zero either way: it is a surplus tally,
		// not a backlog, and letting it pile up across a paced stretch would
		// hand the first unpaced drain a bound it never earned.
		const uint32_t rp = g_repaint_presents_since_app.exchange(0, std::memory_order_relaxed);
		if (rp > 0 && !g_app_paced) {
			uint32_t drained = 0;
			while (drained < rp + LATE_WEAVE_MAX_DEPTH &&
			       WaitForSingleObjectEx(g_frame_latency_waitable, 0, FALSE) == WAIT_OBJECT_0) {
				drained++;
			}
			g_app_drained.fetch_add(drained, std::memory_order_relaxed);
			static bool logged_drain = false;
			if (!logged_drain && drained > 2) {
				logged_drain = true;
				U_LOG_W("Late-weave: drained %u surplus waitable tokens after repaints "
				        "(#868 interplay; logged once)",
				        drained);
			}
		}
		// #2564: take back tokens a governor depth decrease left behind.
		g_lw_ledger_d3d11.pay(g_frame_latency_waitable, "d3d11");
		const uint64_t wait_t0 = os_monotonic_get_ns();
		if (g_app_holds_token) {
			// #1571: the previous app frame took a token here and then
			// SKIPPED its present (#918 F4). Nothing retired, so nothing
			// was released — that token is still ours and no wait can
			// produce another. Reuse it. Waiting instead is the bug: the
			// full 100 ms bound, every frame, for the life of the chain.
			wait_timed_out = false;
		} else {
			wait_timed_out = WaitForSingleObject(g_frame_latency_waitable, 100) == WAIT_TIMEOUT;
			// A timeout took nothing, so there is nothing to carry.
			g_app_holds_token = !wait_timed_out;
		}
		const uint64_t wait_t1 = os_monotonic_get_ns();
		const bool wait_blocked = (wait_t1 - wait_t0) > 2000000; // >2 ms
		// #1482: the two ends of the stage-1 distribution, for the trace row. A
		// full-bound timeout is this bug's signature; an instant return is
		// #868's. Diagnostics only, never a control input.
		if (wait_timed_out) {
			g_app_wait_timeouts.fetch_add(1, std::memory_order_relaxed);
		} else if (!wait_blocked) {
			g_app_wait_instant.fetch_add(1, std::memory_order_relaxed);
		}
		// #1339: same two stamps, also kept as a peak for the trace row.
		// Only this thread raises the peak (the reader only exchanges it to
		// zero), so a load-compare-store needs no CAS.
		const uint64_t app_wait_ns = wait_t1 - wait_t0;
		if (app_wait_ns > g_app_wait_max_ns.load(std::memory_order_relaxed)) {
			g_app_wait_max_ns.store(app_wait_ns, std::memory_order_relaxed);
		}
		// Catch-up guard: if the LAST frame overran its period the pipeline
		// is already a vsync behind — tick-aligning now waits for yet
		// another tick and turns one late pickup into a dropped frame
		// (measured on the VK bridge: align-always put 20% of frames at
		// 2 vsyncs, 60 -> 51 fps, identical per-frame GPU cost). Skip the
		// align once; the queue slack absorbs the late frame and the next
		// on-time frame re-aligns.
		static uint64_t s_last_pace_done_ns;
		const double cu_period_ns =
		    (g_lw_gov_d3d11.period_ns > 0.0) ? g_lw_gov_d3d11.period_ns : 16.7e6;
		const bool running_late = s_last_pace_done_ns != 0 &&
		                          (double)(wait_t1 - s_last_pace_done_ns) > cu_period_ns * 1.15;

		// #1482 instrumentation: stage 2 was the remaining unmeasured hole. The
		// opaque chain's scanout wait is bounded at three panel periods (50 ms
		// at 60 Hz — a whole stride at D=3) and runs under the same lock as the
		// stage-1 wait above, so it needs a peak of its own.
		const uint64_t stage2_t0 = os_monotonic_get_ns();
		if (g_target_is_composed) {
			// Composed chain: the weave should start at a constant, small
			// offset after a compositor tick. A BLOCKING waitable release
			// already lands at a tick (DWM pickup is tick-aligned) — a
			// clock wait on top would add a whole period (measured: frame
			// interval 2x period, spurious governor backoff). Only when
			// the wait returned instantly (banked token, unknown phase)
			// does the clock alignment buy the constant phase.
			static bool logged_src = false;
			bool clocked = false;
			if (!wait_blocked && !wait_timed_out && !running_late && g_lw_gov_d3d11.align_ok()) {
				clocked = late_weave_wait_compositor_clock(g_lw_gov_d3d11.period_ns);
			}
			if (!logged_src) {
				logged_src = true;
				U_LOG_W("Late-weave: live-path stage-2 source = %s",
				        (clocked || wait_blocked) ? "DComp compositor clock / tick-aligned pickup"
				                                  : "none (waitable only)");
			}
		} else if (g_scanout_stats_strikes >= 0 && !g_lw_gov_d3d11.in_resize_hold() &&
		           g_lw_stage2_d3d11.allowed(target->swapchain, "d3d11")) {
			// #2564: not during a resize. ResizeBuffers makes the frame
			// statistics disjoint, so this wait ran to its 3-period bound
			// on most drag steps (a ~50 ms hitch each, 40 fps for the whole
			// drag), and those timeouts are not evidence against the chain:
			// they must not count toward the strike gate below either.
			//
			// At effective depth L>1 (#850) the pacer targets the present
			// L-1 back instead, restoring L-1 frames of CPU/GPU overlap.
			const UINT relax = (UINT)(g_lw_gov_d3d11.effective - 1);
			if (g_last_present_count > relax) {
				const enum late_weave_scanout_result r = late_weave_wait_scanout(
				    target->swapchain, g_last_present_count - relax,
				    g_lw_gov_d3d11.period_ns, g_weave_latency_d3d11.freq());
				// Behavioral trust gate: stats that succeed but never
				// advance burn the full 3-period bound every frame —
				// declare them unusable after a few strikes.
				if (r == LATE_WEAVE_SCANOUT_TIMED_OUT) {
					if (++g_scanout_stats_strikes >= 8) {
						g_scanout_stats_strikes = -1;
						U_LOG_W("Late-weave: frame statistics stale on this "
						        "chain — stage-2 scanout wait disabled");
					}
				} else if (r == LATE_WEAVE_SCANOUT_REACHED) {
					g_scanout_stats_strikes = 0;
				}
			}
		}
		s_last_pace_done_ns = os_monotonic_get_ns();
		const uint64_t stage2_ns = s_last_pace_done_ns - stage2_t0;
		if (stage2_ns > g_app_stage2_max_ns.load(std::memory_order_relaxed)) {
			g_app_stage2_max_ns.store(stage2_ns, std::memory_order_relaxed);
		}
	}
	g_weave_latency_d3d11.mark_weave("d3d11", predicted_display_time_ns);
	if (wait_timed_out) {
		// DWM stopped pickup (occlusion/minimize) — not saturation. Without
		// this the ~100 ms timeout is below the governor's 250 ms pause
		// threshold and reads as a saturated pipeline, pinning max depth.
		g_lw_gov_d3d11.on_wait_timeout();
	}
	if (g_frame_latency_waitable != nullptr) {
		const int tr = g_lw_gov_d3d11.on_mark(g_weave_latency_d3d11.freq());
		if (tr != 0) {
			IDXGISwapChain2 *sc2 = nullptr;
			if (SUCCEEDED(target->swapchain->QueryInterface(
			        __uuidof(IDXGISwapChain2), reinterpret_cast<void **>(&sc2)))) {
				g_lw_ledger_d3d11.apply(sc2, g_lw_gov_d3d11.effective);
				sc2->Release();
			}
			static bool logged_backoff = false, logged_return = false;
			bool &logged = (tr > 0) ? logged_backoff : logged_return;
			if (!logged) {
				logged = true;
				U_LOG_W("Late-weave: saturation %s -> max latency %d "
				        "(frame interval %.1f ms vs display period %.1f ms)",
				        tr > 0 ? "backoff engaged" : "cleared, probing return",
				        g_lw_gov_d3d11.effective, g_lw_gov_d3d11.interval_ema_ns / 1e6,
				        g_lw_gov_d3d11.period_ns / 1e6);
			}
		}
	}
}

extern "C" void
comp_d3d11_target_mark_wait_frame(struct comp_d3d11_target *target)
{
	(void)target;
	g_lw_gov_d3d11.on_wait_frame();
}

extern "C" uint64_t
comp_d3d11_target_get_predicted_lookahead_ns(struct comp_d3d11_target *target)
{
	(void)target;
	return g_lw_gov_d3d11.predicted_lookahead_ns(g_weave_latency_d3d11.measured_r_ns);
}

extern "C" void
comp_d3d11_target_set_display_period(struct comp_d3d11_target *target, uint64_t period_ns)
{
	(void)target;
	// Seed the governor from the compositor's queried refresh rate so the
	// first frames judge saturation correctly; DXGI frame statistics refine
	// it from there.
	if (period_ns > 0 && g_lw_gov_d3d11.period_ns == 0.0) {
		g_lw_gov_d3d11.period_ns = (double)period_ns;
	}
}

extern "C" void
comp_d3d11_target_set_app_paced(struct comp_d3d11_target *target, bool paced)
{
	(void)target;
	g_app_paced = paced;
}

extern "C" uint64_t
comp_d3d11_target_get_display_period_ns(struct comp_d3d11_target *target)
{
	(void)target;
	// The governor seeds this from the compositor's queried refresh rate and
	// refines it from DXGI frame statistics, so it is the one place on this
	// target that knows the real period. <= 0 means "not measured yet", which
	// the DP contract spells "0 = unknown".
	const double p = g_lw_gov_d3d11.period_ns;
	return (p > 0.0) ? (uint64_t)p : 0;
}

extern "C" uint64_t
comp_d3d11_target_get_measured_weave_ns(struct comp_d3d11_target *target)
{
	(void)target;
	return g_weave_latency_d3d11.measured_weave_ns_fresh();
}

extern "C" uint64_t
comp_d3d11_target_predict_weave_to_scanout_ns(struct comp_d3d11_target *target)
{
	(void)target;
	return g_weave_latency_d3d11.predict_weave_to_scanout_ns();
}

extern "C" xrt_result_t
comp_d3d11_target_present(struct comp_d3d11_target *target, uint32_t sync_interval)
{
	// #925 S1 (audit rank 1): a plain Present(1,0) into a jammed flip chain
	// blocks for MINUTES with c->mutex held — the in-process twin of the
	// #924 service wedge (hybrid NV→Intel present path observed live).
	// This Present is deliberately the frame pacer (device max latency 1 →
	// Present(1) waits ≤1 vsync when healthy), so preserve the pacing shape
	// but bound the wedge: non-blocking present retried at ~1 ms cadence up
	// to a 50 ms (~3 vsync) deadline, then drop the frame. A jammed chain
	// degrades to slow frames; the app's render thread (and its IPC) stays
	// alive. Mirrors the service's standalone commit path.
	// Lost device: no Present into the dead chain (it would fail anyway and,
	// before this latch, logged an ERROR per frame).
	if (target->device_lost) {
		return XRT_ERROR_D3D;
	}
	// #2635: on the app's own thread (never the repaint thread), check whether
	// it is calling us from inside a Present hook that also hooks this chain.
	// Before the first repaint mark g_repaint_thread_id is 0, so every
	// present until then is an app present.
	if (GetCurrentThreadId() != g_repaint_thread_id) {
		target_reentry_sample(target);
	}
	// #2635: under a re-entrant Present hook the call must be made on THIS
	// thread. The #2603 worker would enter the hook on another thread and wait
	// for the lock this (app) thread holds while this thread waits for the
	// worker: a deadlock that #2603 then misreads as a driver wedge (measured:
	// session lost 5 s after the first frame). Inline gives up #2603's bound
	// for this session only; a real driver wedge here hangs as before #2603.
	const bool inline_present = target->pw_thread == nullptr || target->present_reentrant;
	if (!target->present_thread_logged) {
		target->present_thread_logged = true;
		U_LOG_W("d3d11 target: first Present — caller tid=%lu, presenting tid=%lu (%s), present-hook "
		        "re-entry=%d (#2635)",
		        (unsigned long)GetCurrentThreadId(), (unsigned long)target_presenting_tid(target, inline_present),
		        inline_present ? "inline" : "#2603 bounded worker", target->present_reentrant ? 1 : 0);
	}
	if (target->present_reentrant && target->pw_thread != nullptr && !target->reentry_inline_logged) {
		target->reentry_inline_logged = true;
		U_LOG_W("d3d11 target: present-hook re-entry — Present moves off the #2603 worker (tid=%lu) onto "
		        "the app's thread (tid=%lu) for the rest of the session (#2635)",
		        (unsigned long)GetThreadId(target->pw_thread), (unsigned long)GetCurrentThreadId());
	}
	HRESULT hr;
	if (!inline_present) {
		// #2603: the call runs on the bounded worker; this thread waits for
		// it exactly as long as it takes, up to the bound. Anything but a
		// clean done (a timeout, or a failed wait with the worker possibly
		// still presenting) is treated as wedged: never touch the chain again.
		target->pw_sync_interval = sync_interval;
		SetEvent(target->pw_req);
		if (WaitForSingleObject(target->pw_done, target->present_bound_ms) != WAIT_OBJECT_0) {
			U_LOG_W("d3d11 target: wedge — caller tid=%lu waited on the #2603 worker tid=%lu; "
			        "present-hook re-entry=%d (#2635/#2603)",
			        (unsigned long)GetCurrentThreadId(), (unsigned long)GetThreadId(target->pw_thread),
			        target->present_reentrant ? 1 : 0);
			target_mark_present_wedged(target);
			return XRT_ERROR_D3D;
		}
		hr = target->pw_hr;
	} else {
		hr = target_present_call(target, sync_interval);
	}
	if (SUCCEEDED(hr)) {
		// Witness counts frames that actually reached the chain — the 50 ms
		// drop path above intentionally shows up as a lower weave/s.
		g_frame_witness_d3d11.count_present();
	}
	// #1571: the APP's token settles on a present that actually went out.
	// Thread-keyed for the mirror image of the reason below: a repaint present
	// must not settle the app's carry. A dropped present (FAILED, or the 50 ms
	// deadline) released nothing, so the carry survives it and the next
	// weave_mark reuses the token instead of blocking on one that will never
	// come. g_repaint_thread_id is 0 until the first repaint mark, so app
	// presents before that still take this branch.
	if (SUCCEEDED(hr) && GetCurrentThreadId() != g_repaint_thread_id) {
		g_app_holds_token = false;
	}
	// #1339: a repaint's present settles its token. Thread-keyed, because an
	// app present can land between a bailed repaint's mark and its next tick
	// and must not settle on the repaint's behalf. A dropped present (FAILED,
	// or the 50 ms deadline) released nothing: the held token stays held and
	// admit reuses it; an un-capped repaint that dropped is not counted for
	// the app-side drain either.
	if (g_repaint_present_pending && GetCurrentThreadId() == g_repaint_thread_id) {
		g_repaint_present_pending = false;
		if (SUCCEEDED(hr)) {
			if (g_repaint_holds_token) {
				g_repaint_holds_token = false;
			} else {
				// Old accounting (cap off, or no waitable): this present
				// releases a token nobody waited for; the app's weave_mark
				// drains it (#868 interplay).
				g_repaint_presents_since_app.fetch_add(1, std::memory_order_relaxed);
			}
		}
	}
	g_weave_latency_d3d11.after_present("d3d11", target->swapchain, &g_lw_gov_d3d11);
	if (SUCCEEDED(hr) && g_frame_latency_waitable != nullptr) {
		target->swapchain->GetLastPresentCount(&g_last_present_count);
	}
	if (FAILED(hr)) {
		// A removed/reset device latches and logs once (with the reason);
		// anything else is rate-limited — never an ERROR per frame.
		if (!target_check_device_lost(target, hr, "Present")) {
			static std::atomic<int64_t> s_last_fail_log_ns{0};
			const int64_t now_ns = (int64_t)os_monotonic_get_ns();
			int64_t prev_ns = s_last_fail_log_ns.load(std::memory_order_relaxed);
			if (now_ns - prev_ns > 1000000000LL &&
			    s_last_fail_log_ns.compare_exchange_strong(prev_ns, now_ns)) {
				U_LOG_E("Present failed: 0x%08x (rate-limited, at most once per second)", (unsigned)hr);
			}
		}
		return XRT_ERROR_D3D;
	}

	// DComp path: publish the new frame to dwm.exe. Cheap — IPC of delta state, no GPU work.
	if (target->dcomp_device != nullptr) {
		target->dcomp_device->Commit();
	}

	return XRT_SUCCESS;
}

extern "C" void
comp_d3d11_target_get_dimensions(struct comp_d3d11_target *target,
                                 uint32_t *out_width,
                                 uint32_t *out_height)
{
	*out_width = target->width;
	*out_height = target->height;
}

extern "C" void *
comp_d3d11_target_get_back_buffer(struct comp_d3d11_target *target)
{
	if (target == nullptr) {
		return nullptr;
	}
	return target->back_buffer;
}

extern "C" void *
comp_d3d11_target_get_rtv(struct comp_d3d11_target *target)
{
	if (target == nullptr) {
		return nullptr;
	}
	return target->rtv;
}

extern "C" xrt_result_t
comp_d3d11_target_resize(struct comp_d3d11_target *target,
                         uint32_t width,
                         uint32_t height)
{
	// Lost device: ResizeBuffers on it can only fail. The caller sees the
	// failure and @ref comp_d3d11_target_is_device_lost says why.
	if (target->device_lost) {
		return XRT_ERROR_D3D;
	}
	// Same size is a no-op ONLY while the chain still has its buffers: a
	// failed resize released them, and returning to the old size must still
	// run ResizeBuffers + create_rtv, or the target stays bufferless.
	if (width == target->width && height == target->height && target->rtv != nullptr) {
		return XRT_SUCCESS;
	}

	// #2564: a resize drag's frame cadence must not drive the late-weave
	// governor (see late_weave_governor::on_resize).
	if (g_frame_latency_waitable != nullptr) {
		g_lw_gov_d3d11.on_resize(g_weave_latency_d3d11.freq());
	}

	// Release current back buffer and RTV
	target->ctx->OMSetRenderTargets(0, nullptr, nullptr);
	if (target->rtv != nullptr) {
		target->rtv->Release();
		target->rtv = nullptr;
	}
	if (target->back_buffer != nullptr) {
		target->back_buffer->Release();
		target->back_buffer = nullptr;
	}

	// Resize swapchain. Must pass the creation flags — DXGI returns
	// E_INVALIDARG if they differ (#848, waitable late-weave chains).
	HRESULT hr = target->swapchain->ResizeBuffers(0, width, height,
	                                               DXGI_FORMAT_UNKNOWN,
	                                               target->swapchain_flags);
	if (FAILED(hr)) {
		if (!target_check_device_lost(target, hr, "ResizeBuffers")) {
			U_LOG_E("Failed to resize swapchain: 0x%08x", (unsigned)hr);
		}
		return XRT_ERROR_D3D;
	}

	target->width = width;
	target->height = height;

	// Recreate RTV
	return create_rtv(target);
}
