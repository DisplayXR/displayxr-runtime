// Copyright 2025-2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime's WS_CHILD present window (see comp_child_window_win.h).
 *
 * Moved here from the D3D12 target's E_ACCESSDENIED fallback so the D3D11
 * target can share it. Behaviour unchanged; the window class is renamed.
 *
 * @ingroup comp_util
 */

#include "comp_child_window_win.h"

#include "util/u_logging.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>

struct comp_child_window_win
{
	HWND parent_hwnd;
	HWND child_hwnd;
	uint32_t width;
	uint32_t height;
	HANDLE ready_event;
	HANDLE thread_handle;
};

static const wchar_t *CHILD_WINDOW_CLASS = L"DisplayXRChildPresentWindow";

static LRESULT CALLBACK
child_wnd_proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_DESTROY: PostQuitMessage(0); return 0;
	default: return DefWindowProcW(hWnd, message, wParam, lParam);
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
	struct comp_child_window_win *ctx = (struct comp_child_window_win *)param;

	ensure_child_window_class_registered();

	ctx->child_hwnd =
	    CreateWindowExW(WS_EX_NOPARENTNOTIFY, CHILD_WINDOW_CLASS, L"", WS_CHILD | WS_VISIBLE, 0, 0, (int)ctx->width,
	                    (int)ctx->height, ctx->parent_hwnd, NULL, GetModuleHandleW(NULL), NULL);

	SetEvent(ctx->ready_event);

	if (ctx->child_hwnd == nullptr) {
		U_LOG_E("Child window thread: CreateWindowExW failed: %lu", GetLastError());
		return 1;
	}

	U_LOG_I("Child window thread: HWND=%p (parent=%p), running message loop", (void *)ctx->child_hwnd,
	        (void *)ctx->parent_hwnd);

	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	U_LOG_I("Child window thread: exiting");
	return 0;
}

extern "C" struct comp_child_window_win *
comp_child_window_win_create(void *parent_hwnd, uint32_t width, uint32_t height)
{
	auto *ctx = new comp_child_window_win();
	memset(ctx, 0, sizeof(*ctx));
	ctx->parent_hwnd = static_cast<HWND>(parent_hwnd);
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
	// CreateWindowExW(WS_CHILD) on the child thread can send to the parent's
	// thread (this thread) via SendMessage. If we block with
	// WaitForSingleObject, that SendMessage deadlocks.
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
			break; // Event signaled: child window created
		}
		if (wait == WAIT_OBJECT_0 + 1) {
			// Sent message pending: dispatch it and loop
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

extern "C" void *
comp_child_window_win_hwnd(const struct comp_child_window_win *child)
{
	return child != nullptr ? (void *)child->child_hwnd : nullptr;
}

extern "C" void
comp_child_window_win_resize(struct comp_child_window_win *child, uint32_t width, uint32_t height)
{
	if (child == nullptr || child->child_hwnd == nullptr) {
		return;
	}
	child->width = width;
	child->height = height;
	MoveWindow(child->child_hwnd, 0, 0, (int)width, (int)height, TRUE);
}

extern "C" void
comp_child_window_win_destroy(struct comp_child_window_win **child_ptr)
{
	if (child_ptr == nullptr || *child_ptr == nullptr)
		return;

	struct comp_child_window_win *ctx = *child_ptr;

	if (ctx->child_hwnd != nullptr) {
		PostMessage(ctx->child_hwnd, WM_CLOSE, 0, 0);
	}
	if (ctx->thread_handle != nullptr) {
		WaitForSingleObject(ctx->thread_handle, 5000);
		CloseHandle(ctx->thread_handle);
	}

	delete ctx;
	*child_ptr = nullptr;
}
