// Copyright 2025, Leia Inc.
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

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

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

	//! Child window context (non-NULL only when child window fallback is active).
	struct child_window_context *child_ctx;

	//! Current dimensions.
	uint32_t width;
	uint32_t height;
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
                         struct comp_d3d12_target **out_target)
{
	auto internals = get_internals(c);

	comp_d3d12_target *target = new comp_d3d12_target();
	memset(target, 0, sizeof(*target));
	target->c = c;
	target->hwnd = static_cast<HWND>(hwnd);
	target->width = width;
	target->height = height;

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
	// IGNORE so DWM doesn't composite the desktop through the bound HWND (#163).
	desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	desc.Flags = 0;

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

	// Create swapchain using the command queue (D3D12 requirement)
	IDXGISwapChain1 *swapchain1 = nullptr;
	HWND swapchain_hwnd = target->hwnd;

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

	// Disable Alt-Enter fullscreen toggle
	dxgi_factory->MakeWindowAssociation(swapchain_hwnd, DXGI_MWA_NO_ALT_ENTER);
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

	// Acquire back buffers and create RTVs
	xrt_result_t xret = acquire_back_buffers(target);
	if (xret != XRT_SUCCESS) {
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
	if (target->swapchain != nullptr) {
		target->swapchain->Release();
	}

	destroy_child_window_threaded(&target->child_ctx);

	delete target;
	*target_ptr = nullptr;
}

extern "C" xrt_result_t
comp_d3d12_target_present(struct comp_d3d12_target *target, uint32_t sync_interval)
{
	HRESULT hr = target->swapchain->Present(sync_interval, 0);
	if (FAILED(hr)) {
		U_LOG_E("Present failed: 0x%08x", hr);
		return XRT_ERROR_D3D;
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

	// Resize swapchain
	HRESULT hr = target->swapchain->ResizeBuffers(BACK_BUFFER_COUNT, width, height,
	                                               DXGI_FORMAT_UNKNOWN, 0);
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
