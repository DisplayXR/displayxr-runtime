// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D12 DXGI swapchain target implementation.
 * @author David Fattal
 * @ingroup comp_d3d12
 */

#include "comp_d3d12_target.h"
#include "comp_d3d12_compositor.h"

#include "util/u_logging.h"
#include "util/u_debug.h"

#include "os/os_time.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dcomp.h>

// Decoupled presentation (#833): with DXR_PRESENT_OPAQUE=1 a transparent
// session presents through the opaque HWND flip-model path while the DP
// keeps compose-under-bg - the baked desktop makes the output opaque by
// construction, and the HWND flip swapchain is eligible for Hardware
// Independent Flip (DComp visuals stay at full Composed: Flip cost
// regardless of their alpha mode). See the use_transparent gate below.
DEBUG_GET_ONCE_BOOL_OPTION(present_opaque, "DXR_PRESENT_OPAQUE", false)

#include "util/comp_weave_latency_win.h"

// One in-process D3D12 target per compositor per app process — file-scope
// harness instance (the target struct is memset after construction, which
// would clobber the logger's -1 sentinel).
static weave_latency_log g_weave_latency_d3d12;

// Latency governor (#850): DXR_LATE_WEAVE_MAX_LATENCY knob + saturation
// auto-backoff. Lives at file scope for the same memset reason as the log.
static late_weave_governor g_lw_gov_d3d12;

// Late-weave (DXR_LATE_WEAVE=1): same gate as the D3D11 service / VK native
// compositors — waitable swapchain, max frame latency 1, weave paced to the
// waitable so the eye prediction is ~1 refresh old at scanout.
static bool
dxr_late_weave_enabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		// Default ON: late-weave is the product behavior on every path
		// (measured 96->17 ms VK, 62->17 D3D12, 32->17 D3D11, 29->17
		// workspace). DXR_LATE_WEAVE=0 opts out for A/B or triage.
		const char *e = getenv("DXR_LATE_WEAVE");
		enabled = (e != nullptr && e[0] == '0') ? 0 : 1;
	}
	return enabled == 1;
}

#define BACK_BUFFER_COUNT 3

/*!
 * D3D12 target structure.
 */
struct comp_d3d12_target
{
	//! Parent compositor.
	struct comp_d3d12_compositor *c;

	//! DXGI swapchain.
	IDXGISwapChain3 *swapchain;

	//! Back buffer resources.
	ID3D12Resource *back_buffers[BACK_BUFFER_COUNT];

	//! RTV descriptor heap for back buffers.
	ID3D12DescriptorHeap *rtv_heap;

	//! RTV descriptor size.
	uint32_t rtv_descriptor_size;

	//! Window handle.
	HWND hwnd;

	//! Late-weave (DXR_LATE_WEAVE=1): frame-latency waitable from the
	//! swapchain (FRAME_LATENCY_WAITABLE_OBJECT + max latency 1). The
	//! compositor waits on it right before the weave, vsync-locking
	//! weave+present and capping the present queue at 1 (default here was
	//! bc=3 with NO latency cap). NULL when late-weave is off or the
	//! transparent DComp path is active.
	HANDLE frame_latency_waitable;

	//! Last Present()'s DXGI present count — the scanout pacer in
	//! weave_mark waits until GetFrameStatistics reports it flipped to
	//! glass. The waitable alone is NOT enough on composed (windowed)
	//! presents: it releases on DWM *pickup*, 2-3 frames before scanout
	//! (measured: waitable-only left R at ~62 ms on this path).
	UINT last_present_count;

	//! Child window context (non-NULL only when child window fallback is active).
	struct child_window_context *child_ctx;

	//! Current dimensions.
	uint32_t width;
	uint32_t height;

	//! Swapchain creation flags — DXGI requires ResizeBuffers to pass the
	//! exact same flags or it fails with E_INVALIDARG (#848; hit whenever
	//! the late-weave FRAME_LATENCY_WAITABLE_OBJECT chain is resized).
	UINT swapchain_flags;

	//! DirectComposition resources (transparent path only — null on default path).
	//! D3D12 forbids DXGI_SWAP_EFFECT_DISCARD, so the transparent path uses
	//! CreateSwapChainForComposition + flip-model + DXGI_ALPHA_MODE_PREMULTIPLIED
	//! and binds the swapchain to the HWND through DComp instead of via DXGI.
	IDCompositionDevice *dcomp_device;
	IDCompositionTarget *dcomp_target;
	IDCompositionVisual *dcomp_visual;
};

// Access compositor internals
extern "C" {
struct comp_d3d12_compositor_internals
{
	struct xrt_compositor_native base;
	struct xrt_device *xdev;
	ID3D12Device *device;
	ID3D12CommandQueue *command_queue;
};
}

static inline struct comp_d3d12_compositor_internals *
get_internals(struct comp_d3d12_compositor *c)
{
	return reinterpret_cast<struct comp_d3d12_compositor_internals *>(c);
}

static void
release_back_buffers(struct comp_d3d12_target *target)
{
	for (uint32_t i = 0; i < BACK_BUFFER_COUNT; i++) {
		if (target->back_buffers[i] != nullptr) {
			target->back_buffers[i]->Release();
			target->back_buffers[i] = nullptr;
		}
	}
}

/*
 *
 * Child window with dedicated message-pump thread.
 *
 * When the E_ACCESSDENIED fallback creates a WS_CHILD window, it must live on
 * its own thread with a GetMessage loop. Otherwise, DXGI Present() deadlocks
 * when the parent window enters a modal resize/move loop (the parent's UI
 * thread is blocked, and Present() tries to SendMessage to it).
 *
 */

struct child_window_context
{
	HWND parent_hwnd;
	HWND child_hwnd;
	uint32_t width;
	uint32_t height;
	HANDLE ready_event;
	HANDLE thread_handle;
};

static const wchar_t *CHILD_WINDOW_CLASS = L"CompD3D12ChildWindow";

static LRESULT CALLBACK
child_wnd_proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	default:
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
}

static bool
ensure_child_window_class_registered()
{
	static bool registered = false;
	if (registered)
		return true;

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = child_wnd_proc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = CHILD_WINDOW_CLASS;

	if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		return false;
	registered = true;
	return true;
}

static DWORD WINAPI
child_window_thread_func(LPVOID param)
{
	struct child_window_context *ctx = (struct child_window_context *)param;

	ensure_child_window_class_registered();

	ctx->child_hwnd = CreateWindowExW(
	    WS_EX_NOPARENTNOTIFY, CHILD_WINDOW_CLASS, L"",
	    WS_CHILD | WS_VISIBLE,
	    0, 0, (int)ctx->width, (int)ctx->height,
	    ctx->parent_hwnd, NULL, GetModuleHandleW(NULL), NULL);

	SetEvent(ctx->ready_event);

	if (ctx->child_hwnd == nullptr) {
		U_LOG_E("Child window thread: CreateWindowExW failed: %lu", GetLastError());
		return 1;
	}

	U_LOG_I("Child window thread: HWND=%p (parent=%p), running message loop",
	        (void *)ctx->child_hwnd, (void *)ctx->parent_hwnd);

	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	U_LOG_I("Child window thread: exiting");
	return 0;
}

static struct child_window_context *
create_child_window_threaded(HWND parent, uint32_t width, uint32_t height)
{
	auto *ctx = new child_window_context();
	memset(ctx, 0, sizeof(*ctx));
	ctx->parent_hwnd = parent;
	ctx->width = width;
	ctx->height = height;

	ctx->ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (ctx->ready_event == NULL) {
		U_LOG_E("Failed to create child window ready event");
		delete ctx;
		return nullptr;
	}

	ctx->thread_handle = CreateThread(NULL, 0, child_window_thread_func, ctx, 0, NULL);
	if (ctx->thread_handle == NULL) {
		U_LOG_E("Failed to create child window thread");
		CloseHandle(ctx->ready_event);
		delete ctx;
		return nullptr;
	}

	// Wait for the child window thread, but pump sent messages.
	// CreateWindowExW(WS_CHILD) on the child thread sends WM_PARENTNOTIFY
	// to the parent's thread (this thread) via SendMessage. If we block
	// with WaitForSingleObject, that SendMessage deadlocks.
	// MsgWaitForMultipleObjects + QS_SENDMESSAGE lets us process it.
	DWORD wait;
	DWORD deadline = GetTickCount() + 5000;
	for (;;) {
		DWORD remaining = deadline - GetTickCount();
		if ((int)remaining <= 0) {
			wait = WAIT_TIMEOUT;
			break;
		}
		wait = MsgWaitForMultipleObjects(1, &ctx->ready_event, FALSE, remaining, QS_SENDMESSAGE);
		if (wait == WAIT_OBJECT_0) {
			break; // Event signaled — child window created
		}
		if (wait == WAIT_OBJECT_0 + 1) {
			// Sent message pending — dispatch it and loop
			MSG msg;
			while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE | PM_QS_SENDMESSAGE)) {
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			continue;
		}
		// Timeout or error
		break;
	}
	CloseHandle(ctx->ready_event);
	ctx->ready_event = NULL;

	if (wait != WAIT_OBJECT_0 || ctx->child_hwnd == nullptr) {
		U_LOG_E("Child window thread failed to create HWND");
		WaitForSingleObject(ctx->thread_handle, 1000);
		CloseHandle(ctx->thread_handle);
		delete ctx;
		return nullptr;
	}

	return ctx;
}

static void
destroy_child_window_threaded(struct child_window_context **ctx_ptr)
{
	if (ctx_ptr == nullptr || *ctx_ptr == nullptr)
		return;

	struct child_window_context *ctx = *ctx_ptr;

	if (ctx->child_hwnd != nullptr) {
		PostMessage(ctx->child_hwnd, WM_CLOSE, 0, 0);
	}
	if (ctx->thread_handle != nullptr) {
		WaitForSingleObject(ctx->thread_handle, 5000);
		CloseHandle(ctx->thread_handle);
	}

	delete ctx;
	*ctx_ptr = nullptr;
}

static xrt_result_t
acquire_back_buffers(struct comp_d3d12_target *target)
{
	for (uint32_t i = 0; i < BACK_BUFFER_COUNT; i++) {
		HRESULT hr = target->swapchain->GetBuffer(i, __uuidof(ID3D12Resource),
		                                           reinterpret_cast<void **>(&target->back_buffers[i]));
		if (FAILED(hr)) {
			U_LOG_E("Failed to get back buffer %u: 0x%08x", i, hr);
			return XRT_ERROR_D3D;
		}
	}

	// Create RTVs for each back buffer
	auto internals = get_internals(target->c);
	D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = target->rtv_heap->GetCPUDescriptorHandleForHeapStart();

	for (uint32_t i = 0; i < BACK_BUFFER_COUNT; i++) {
		internals->device->CreateRenderTargetView(target->back_buffers[i], nullptr, rtv_handle);
		rtv_handle.ptr += target->rtv_descriptor_size;
	}

	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d12_target_create(struct comp_d3d12_compositor *c,
                         void *hwnd,
                         uint32_t width,
                         uint32_t height,
                         bool transparent,
                         struct comp_d3d12_target **out_target)
{
	auto internals = get_internals(c);

	comp_d3d12_target *target = new comp_d3d12_target();
	memset(target, 0, sizeof(*target));
	target->c = c;
	target->hwnd = static_cast<HWND>(hwnd);
	target->width = width;
	target->height = height;

	// Default: flip-model + ALPHA_MODE_IGNORE (#163) — opaque present, no DWM bleed-through.
	// Transparent opt-in (only when an HWND was provided): DXGI forbids
	// DXGI_SWAP_EFFECT_DISCARD for D3D12 at validation time, so we use
	// DirectComposition + flip-model + DXGI_ALPHA_MODE_PREMULTIPLIED.
	// CreateSwapChainForComposition produces an HWND-less swapchain that DComp
	// binds to the app's HWND via IDCompositionTarget — DWM blends per-pixel.
	// Decoupled presentation (#833): DXR_PRESENT_OPAQUE=1 keeps the DP's
	// compose-under-bg (the compositor still passes transparent=true to the
	// DP) but presents through the opaque HWND flip-model path below — the
	// baked desktop makes the output opaque by construction, and an HWND flip
	// swapchain on the app's WS_EX_NOREDIRECTIONBITMAP window is eligible for
	// Hardware Independent Flip (dwm ~1 % instead of ~32 % measured on Intel
	// iGPUs at identical content and frame rate; DComp visuals stay at full
	// Composed: Flip cost regardless of their alpha mode).
	const bool present_opaque = debug_get_bool_option_present_opaque();
	const bool use_transparent = transparent && hwnd != nullptr && !present_opaque;
	if (transparent && hwnd != nullptr && present_opaque) {
		U_LOG_W("DXR_PRESENT_OPAQUE: HWND flip-model opaque present; DP compose-under-bg keeps "
		        "the transparent look (#833)");
	}

	// Create RTV descriptor heap
	D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
	rtv_heap_desc.NumDescriptors = BACK_BUFFER_COUNT;
	rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

	HRESULT hr = internals->device->CreateDescriptorHeap(
	    &rtv_heap_desc, __uuidof(ID3D12DescriptorHeap),
	    reinterpret_cast<void **>(&target->rtv_heap));
	if (FAILED(hr)) {
		U_LOG_E("Failed to create RTV descriptor heap: 0x%08x", hr);
		delete target;
		return XRT_ERROR_D3D;
	}

	target->rtv_descriptor_size = internals->device->GetDescriptorHandleIncrementSize(
	    D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	// Create DXGI swapchain
	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = BACK_BUFFER_COUNT;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	if (use_transparent) {
		desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
	} else {
		// IGNORE so DWM doesn't composite the desktop through the bound HWND (#163).
		desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	}
	desc.Flags = 0;
	if (dxr_late_weave_enabled() && !use_transparent) {
		desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
	}
	target->swapchain_flags = desc.Flags;

	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs_desc = {};
	fs_desc.Windowed = TRUE;

	// Get DXGI factory.
	// Note: ID3D12Device does NOT implement IDXGIDevice (unlike D3D11),
	// so we must get the factory via the device's adapter LUID.
	IDXGIFactory4 *dxgi_factory = nullptr;
	{
		LUID adapter_luid = internals->device->GetAdapterLuid();
		hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void **>(&dxgi_factory));
		if (FAILED(hr) || dxgi_factory == nullptr) {
			U_LOG_E("Failed to create DXGI factory: 0x%08x", hr);
			target->rtv_heap->Release();
			delete target;
			return XRT_ERROR_D3D;
		}
	}

	// Create swapchain. Default: bound to the app's HWND via CreateSwapChainForHwnd
	// (with the existing E_ACCESSDENIED -> child-window fallback).
	// Transparent: HWND-less via CreateSwapChainForComposition; DComp binds it below.
	IDXGISwapChain1 *swapchain1 = nullptr;
	HWND swapchain_hwnd = target->hwnd;

	if (use_transparent) {
		hr = dxgi_factory->CreateSwapChainForComposition(
		    internals->command_queue, &desc, nullptr, &swapchain1);
		U_LOG_W("Transparent HWND opt-in: DComp + flip-model swapchain "
		        "(FLIP_DISCARD + PREMULTIPLIED, bc=%u)",
		        (unsigned)BACK_BUFFER_COUNT);
	} else {
		hr = dxgi_factory->CreateSwapChainForHwnd(
		    internals->command_queue, swapchain_hwnd, &desc, &fs_desc, nullptr, &swapchain1);

		// HWND already has a swapchain — fall back to child window.
		// DXGI enforces one swapchain per HWND for D3D12, so if the app
		// already created a swapchain on this HWND we get E_ACCESSDENIED.
		if (hr == E_ACCESSDENIED) {
			U_LOG_W("HWND %p already has a swapchain (E_ACCESSDENIED). "
			        "Creating child window on dedicated thread.",
			        (void *)target->hwnd);

			target->child_ctx = create_child_window_threaded(target->hwnd, width, height);
			if (target->child_ctx == nullptr) {
				dxgi_factory->Release();
				target->rtv_heap->Release();
				delete target;
				return XRT_ERROR_D3D;
			}

			swapchain_hwnd = target->child_ctx->child_hwnd;

			hr = dxgi_factory->CreateSwapChainForHwnd(
			    internals->command_queue, swapchain_hwnd, &desc, &fs_desc, nullptr, &swapchain1);
		}

		// Disable Alt-Enter fullscreen toggle (HWND-bound only).
		dxgi_factory->MakeWindowAssociation(swapchain_hwnd, DXGI_MWA_NO_ALT_ENTER);
	}
	dxgi_factory->Release();

	if (FAILED(hr)) {
		U_LOG_E("Failed to create swapchain: 0x%08x", hr);
		destroy_child_window_threaded(&target->child_ctx);
		target->rtv_heap->Release();
		delete target;
		return XRT_ERROR_D3D;
	}

	// QueryInterface for IDXGISwapChain3 (needed for GetCurrentBackBufferIndex)
	hr = swapchain1->QueryInterface(__uuidof(IDXGISwapChain3),
	                                 reinterpret_cast<void **>(&target->swapchain));
	swapchain1->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to get IDXGISwapChain3: 0x%08x", hr);
		destroy_child_window_threaded(&target->child_ctx);
		target->rtv_heap->Release();
		delete target;
		return XRT_ERROR_D3D;
	}

	if ((desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) {
		const int lat = g_lw_gov_d3d12.base_latency();
		target->swapchain->SetMaximumFrameLatency(lat);
		target->frame_latency_waitable = target->swapchain->GetFrameLatencyWaitableObject();
		U_LOG_W("Late-weave: D3D12 swapchain waitable, max latency %d%s (waitable=%p)", lat,
		        (lat == 1 && g_lw_gov_d3d12.auto_backoff == 1) ? " + saturation auto-backoff" : "",
		        target->frame_latency_waitable);
	}

	// Transparent path: bind the composition swapchain to the HWND through DComp.
	// Create device → target-for-HWND → visual → SetContent(swapchain) → SetRoot → Commit.
	if (use_transparent) {
		hr = DCompositionCreateDevice2(
		    /*renderingDevice*/ nullptr,
		    __uuidof(IDCompositionDevice),
		    reinterpret_cast<void **>(&target->dcomp_device));
		if (FAILED(hr) || target->dcomp_device == nullptr) {
			U_LOG_E("DCompositionCreateDevice2 failed: 0x%08x", hr);
			target->swapchain->Release();
			target->rtv_heap->Release();
			delete target;
			return XRT_ERROR_D3D;
		}

		hr = target->dcomp_device->CreateTargetForHwnd(
		    target->hwnd, /*topmost*/ TRUE, &target->dcomp_target);
		if (FAILED(hr) || target->dcomp_target == nullptr) {
			U_LOG_E("IDCompositionDevice::CreateTargetForHwnd failed: 0x%08x", hr);
			target->dcomp_device->Release();
			target->swapchain->Release();
			target->rtv_heap->Release();
			delete target;
			return XRT_ERROR_D3D;
		}

		hr = target->dcomp_device->CreateVisual(&target->dcomp_visual);
		if (FAILED(hr) || target->dcomp_visual == nullptr) {
			U_LOG_E("IDCompositionDevice::CreateVisual failed: 0x%08x", hr);
			target->dcomp_target->Release();
			target->dcomp_device->Release();
			target->swapchain->Release();
			target->rtv_heap->Release();
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
			target->rtv_heap->Release();
			delete target;
			return XRT_ERROR_D3D;
		}
	}

	// Acquire back buffers and create RTVs
	xrt_result_t xret = acquire_back_buffers(target);
	if (xret != XRT_SUCCESS) {
		if (target->dcomp_visual != nullptr) target->dcomp_visual->Release();
		if (target->dcomp_target != nullptr) target->dcomp_target->Release();
		if (target->dcomp_device != nullptr) target->dcomp_device->Release();
		target->swapchain->Release();
		destroy_child_window_threaded(&target->child_ctx);
		target->rtv_heap->Release();
		delete target;
		return xret;
	}

	*out_target = target;

	U_LOG_I("Created D3D12 target: %ux%u", width, height);

	return XRT_SUCCESS;
}

extern "C" void
comp_d3d12_target_destroy(struct comp_d3d12_target **target_ptr)
{
	if (target_ptr == nullptr || *target_ptr == nullptr) {
		return;
	}

	comp_d3d12_target *target = *target_ptr;

	release_back_buffers(target);

	if (target->rtv_heap != nullptr) {
		target->rtv_heap->Release();
	}

	// Release DComp resources before the swapchain (visual holds a swapchain reference;
	// target holds the visual). DWM tears down the on-screen content when the target
	// releases.
	if (target->dcomp_visual != nullptr) {
		target->dcomp_visual->Release();
	}
	if (target->dcomp_target != nullptr) {
		target->dcomp_target->Release();
	}
	if (target->dcomp_device != nullptr) {
		target->dcomp_device->Release();
	}

	if (target->frame_latency_waitable != nullptr) {
		CloseHandle(target->frame_latency_waitable);
		target->frame_latency_waitable = nullptr;
	}

	if (target->swapchain != nullptr) {
		target->swapchain->Release();
	}

	destroy_child_window_threaded(&target->child_ctx);

	delete target;
	*target_ptr = nullptr;
}

/*!
 * #868 repaint mark: pace to the panel exactly like a real frame, but leave the
 * governor and the #867 prediction ledger alone.
 *
 * A repaint re-weaves an unchanged atlas against fresh eye positions. It has no
 * app frame behind it, so:
 *  - it must NOT feed @ref late_weave_governor::on_mark — repaints land one
 *    panel period apart by construction, so counting them would collapse the
 *    saturation EMA and tell the governor a starved pipeline is keeping up.
 *  - it must NOT carry a predicted display time — xrWaitFrame never promised a
 *    photon time for this present, so there is no prediction to score against
 *    it (0 suppresses the D row).
 *
 * The scanout pacing wait IS kept: a repaint that outran the panel would be
 * wasted work, and the wait is what makes the repaint cadence self-limiting.
 */
extern "C" void
comp_d3d12_target_repaint_pace(struct comp_d3d12_target *target)
{
	if (target->frame_latency_waitable != nullptr) {
		// Deliberately NOT WaitForSingleObject(frame_latency_waitable) — the
		// waitable is a SEMAPHORE, and every wait consumes a token that the
		// app's own layer_commit needs. A repaint thread waiting on it throttles
		// the app to roughly half rate even on the ticks where the repaint then
		// bails out and never presents (measured: 31.9 -> 16.9 fps on the Unity
		// avatar with essentially zero repaints actually issued).
		//
		// Scanout pacing alone is enough here, and costs nothing: it only reads
		// DXGI frame statistics until the previous present has flipped.
		const UINT relax = (UINT)(g_lw_gov_d3d12.effective - 1);
		if (target->last_present_count > relax) {
			late_weave_wait_scanout(target->swapchain, target->last_present_count - relax,
			                        g_lw_gov_d3d12.period_ns, g_weave_latency_d3d12.freq());
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
	const double period_ns = g_lw_gov_d3d12.period_ns;
	const double residual_ns = (double)g_weave_latency_d3d12.measured_r_ns;
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

extern "C" void
comp_d3d12_target_weave_mark_repaint(struct comp_d3d12_target *target)
{
	(void)target;
	g_weave_latency_d3d12.mark_weave("d3d12", 0, true);
}

extern "C" void
comp_d3d12_target_weave_mark(struct comp_d3d12_target *target, uint64_t predicted_display_time_ns)
{
	if (target->frame_latency_waitable != nullptr) {
		// Queue cap: with max latency 1 this releases right after DWM picks
		// up the previous present.
		WaitForSingleObject(target->frame_latency_waitable, 100);

		// Scanout pacing: DWM pickup is 2-3 frames before glass on composed
		// presents, so additionally wait until DXGI frame statistics report
		// the previous present actually flipped — the DXGI equivalent of
		// the VK path's vkWaitForPresentKHR pacing. The helper bounds itself
		// to 3 panel periods so an occluded window never wedges us, and
		// yields instead of sleeping on fast panels.
		// At effective depth L>1 (#850) the pacer targets the present L-1
		// back instead, restoring L-1 frames of CPU/GPU overlap.
		const UINT relax = (UINT)(g_lw_gov_d3d12.effective - 1);
		if (target->last_present_count > relax) {
			late_weave_wait_scanout(target->swapchain, target->last_present_count - relax,
			                        g_lw_gov_d3d12.period_ns, g_weave_latency_d3d12.freq());
		}
	}
	g_weave_latency_d3d12.mark_weave("d3d12", predicted_display_time_ns);
	if (target->frame_latency_waitable != nullptr) {
		const int tr = g_lw_gov_d3d12.on_mark(g_weave_latency_d3d12.freq());
		if (tr != 0) {
			target->swapchain->SetMaximumFrameLatency((UINT)g_lw_gov_d3d12.effective);
			static bool logged_backoff = false, logged_return = false;
			bool &logged = (tr > 0) ? logged_backoff : logged_return;
			if (!logged) {
				logged = true;
				U_LOG_W("Late-weave: saturation %s -> max latency %d "
				        "(frame interval %.1f ms vs display period %.1f ms)",
				        tr > 0 ? "backoff engaged" : "cleared, probing return",
				        g_lw_gov_d3d12.effective, g_lw_gov_d3d12.interval_ema_ns / 1e6,
				        g_lw_gov_d3d12.period_ns / 1e6);
			}
		}
	}
}

extern "C" void
comp_d3d12_target_mark_wait_frame(struct comp_d3d12_target *target)
{
	(void)target;
	g_lw_gov_d3d12.on_wait_frame();
}

extern "C" uint64_t
comp_d3d12_target_get_predicted_lookahead_ns(struct comp_d3d12_target *target)
{
	(void)target;
	return g_lw_gov_d3d12.predicted_lookahead_ns(g_weave_latency_d3d12.measured_r_ns);
}

extern "C" void
comp_d3d12_target_set_display_period(struct comp_d3d12_target *target, uint64_t period_ns)
{
	(void)target;
	// Seed the governor from the compositor's queried refresh rate so the
	// first frames judge saturation correctly; DXGI frame statistics refine
	// it from there.
	if (period_ns > 0 && g_lw_gov_d3d12.period_ns == 0.0) {
		g_lw_gov_d3d12.period_ns = (double)period_ns;
	}
}

extern "C" uint64_t
comp_d3d12_target_get_measured_weave_ns(struct comp_d3d12_target *target)
{
	(void)target;
	return g_weave_latency_d3d12.measured_r_ns;
}

extern "C" xrt_result_t
comp_d3d12_target_present(struct comp_d3d12_target *target, uint32_t sync_interval)
{
	HRESULT hr = target->swapchain->Present(sync_interval, 0);
	g_weave_latency_d3d12.after_present("d3d12", target->swapchain, &g_lw_gov_d3d12);
	if (SUCCEEDED(hr) && target->frame_latency_waitable != nullptr) {
		target->swapchain->GetLastPresentCount(&target->last_present_count);
	}
	if (FAILED(hr)) {
		U_LOG_E("Present failed: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// DComp path: publish the new frame to dwm.exe. Cheap — IPC of delta state, no GPU work.
	if (target->dcomp_device != nullptr) {
		target->dcomp_device->Commit();
	}

	return XRT_SUCCESS;
}

extern "C" void
comp_d3d12_target_get_dimensions(struct comp_d3d12_target *target,
                                 uint32_t *out_width,
                                 uint32_t *out_height)
{
	*out_width = target->width;
	*out_height = target->height;
}

extern "C" uint32_t
comp_d3d12_target_get_current_index(struct comp_d3d12_target *target)
{
	return target->swapchain->GetCurrentBackBufferIndex();
}

extern "C" void *
comp_d3d12_target_get_back_buffer(struct comp_d3d12_target *target, uint32_t index)
{
	if (target == nullptr || index >= BACK_BUFFER_COUNT) {
		return nullptr;
	}
	return target->back_buffers[index];
}

extern "C" uint64_t
comp_d3d12_target_get_rtv_handle(struct comp_d3d12_target *target, uint32_t index)
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle = target->rtv_heap->GetCPUDescriptorHandleForHeapStart();
	handle.ptr += static_cast<SIZE_T>(index) * target->rtv_descriptor_size;
	return static_cast<uint64_t>(handle.ptr);
}

extern "C" xrt_result_t
comp_d3d12_target_resize(struct comp_d3d12_target *target,
                         uint32_t width,
                         uint32_t height)
{
	if (width == target->width && height == target->height) {
		return XRT_SUCCESS;
	}

	// Release current back buffers
	release_back_buffers(target);

	// Resize swapchain. Must pass the creation flags — DXGI returns
	// E_INVALIDARG if they differ (#848, waitable late-weave chains).
	HRESULT hr = target->swapchain->ResizeBuffers(BACK_BUFFER_COUNT, width, height,
	                                               DXGI_FORMAT_UNKNOWN, target->swapchain_flags);
	if (FAILED(hr)) {
		U_LOG_E("Failed to resize swapchain: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	target->width = width;
	target->height = height;

	// Re-acquire back buffers
	return acquire_back_buffers(target);
}

extern "C" bool
comp_d3d12_target_has_child_window(struct comp_d3d12_target *target)
{
	return target != nullptr && target->child_ctx != nullptr;
}

extern "C" void
comp_d3d12_target_resize_child_window(struct comp_d3d12_target *target,
                                      uint32_t width,
                                      uint32_t height)
{
	if (target == nullptr || target->child_ctx == nullptr) {
		return;
	}
	MoveWindow(target->child_ctx->child_hwnd, 0, 0, (int)width, (int)height, TRUE);
}
