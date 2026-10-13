// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host, Windows process duties: elevation check,
 *         in-host minidump writer, Restart Manager session-end window.
 * @ingroup conv_host
 */

#include "conv_host_os.h"
#include "conv_host_exit.h"

#include "util/u_logging.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>

#include <stdio.h>
#include <string.h>


/*
 *
 * Elevation.
 *
 */

bool
conv_host_os_is_elevated(char *out_detail, uint32_t detail_size)
{
	HANDLE tok = NULL;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
		if (out_detail != NULL && detail_size > 0) {
			(void)snprintf(out_detail, detail_size, "OpenProcessToken failed (%lu)", GetLastError());
		}
		return false;
	}
	TOKEN_ELEVATION e = {0};
	DWORD len = 0;
	const BOOL ok = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &len);

	DWORD rid = 0;
	union {
		TOKEN_MANDATORY_LABEL tml;
		BYTE buf[64];
	} u;
	if (GetTokenInformation(tok, TokenIntegrityLevel, &u, sizeof(u), &len)) {
		PSID sid = u.tml.Label.Sid;
		rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
	}
	CloseHandle(tok);

	const bool elevated = (ok && e.TokenIsElevated != 0) || rid >= SECURITY_MANDATORY_HIGH_RID;
	if (out_detail != NULL && detail_size > 0) {
		(void)snprintf(out_detail, detail_size, "TokenIsElevated=%lu integrity RID 0x%lx",
		               ok ? (unsigned long)e.TokenIsElevated : 0ul, (unsigned long)rid);
	}
	return elevated;
}


/*
 *
 * Minidump writer.
 *
 */

static wchar_t g_dump_dir[MAX_PATH];
static wchar_t g_dump_tag[64];

typedef BOOL(WINAPI *mini_dump_write_dump_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                             PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                             PMINIDUMP_CALLBACK_INFORMATION);

static LONG WINAPI
crash_filter(EXCEPTION_POINTERS *ep)
{
	// Runs on the faulting thread with a possibly corrupt heap: no CRT
	// allocation, no logging through the sink; one dbghelp call.
	static volatile LONG once = 0;
	if (InterlockedExchange(&once, 1) != 0) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	mini_dump_write_dump_t write_dump =
	    dbghelp != NULL ? (mini_dump_write_dump_t)(void *)GetProcAddress(dbghelp, "MiniDumpWriteDump") : NULL;
	if (write_dump != NULL && g_dump_dir[0] != L'\0') {
		SYSTEMTIME st;
		GetLocalTime(&st);
		wchar_t path[MAX_PATH + 128];
		_snwprintf_s(path, ARRAYSIZE(path), _TRUNCATE,
		             L"%ls\\conversion-host_%ls.%lu_%04u-%02u-%02u_%02u-%02u-%02u.dmp", g_dump_dir, g_dump_tag,
		             (unsigned long)GetCurrentProcessId(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
		             st.wSecond);
		HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (f != INVALID_HANDLE_VALUE) {
			MINIDUMP_EXCEPTION_INFORMATION mei;
			mei.ThreadId = GetCurrentThreadId();
			mei.ExceptionPointers = ep;
			mei.ClientPointers = FALSE;
			const MINIDUMP_TYPE type =
			    (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
			                    MiniDumpWithUnloadedModules);
			(void)write_dump(GetCurrentProcess(), GetCurrentProcessId(), f, type, &mei, NULL, NULL);
			CloseHandle(f);
		}
	}
	// Let the process die with the exception's code ("other = crash").
	return EXCEPTION_EXECUTE_HANDLER;
}

void
conv_host_os_install_crash_handler(const char *provider_id)
{
	wchar_t base[MAX_PATH];
	if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) {
		_snwprintf_s(g_dump_dir, ARRAYSIZE(g_dump_dir), _TRUNCATE, L"%ls\\DisplayXR", base);
		(void)CreateDirectoryW(g_dump_dir, NULL);
		_snwprintf_s(g_dump_dir, ARRAYSIZE(g_dump_dir), _TRUNCATE, L"%ls\\DisplayXR\\dumps", base);
		(void)CreateDirectoryW(g_dump_dir, NULL);
	}
	size_t k = 0;
	for (const char *c = provider_id != NULL ? provider_id : "unknown"; *c != '\0' && k < ARRAYSIZE(g_dump_tag) - 1;
	     c++) {
		const char ch = *c;
		const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
		                ch == '-' || ch == '_';
		g_dump_tag[k++] = ok ? (wchar_t)ch : L'_';
	}
	g_dump_tag[k] = L'\0';

	// No WER dialog, no GP-fault box: the supervisor restarts us.
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	SetUnhandledExceptionFilter(crash_filter);
}


/*
 *
 * Session-end window (Restart Manager, logoff, WM_CLOSE).
 *
 */

static struct
{
	void (*on_close)(void *ctx);
	void *ctx;
	uint32_t backstop_ms;
	volatile LONG requested;
	HWND hwnd;
	HANDLE ready;
} g_se;

static DWORD WINAPI
backstop_thread(LPVOID p)
{
	(void)p;
	Sleep(g_se.backstop_ms);
	U_LOG_W("conversion host: still alive %u ms after a close request; exiting 20 now", g_se.backstop_ms);
	ExitProcess(CONV_HOST_EXIT_UPDATE);
	return 0;
}

static void
request_close(const char *what, LPARAM lParam)
{
	if (InterlockedExchange(&g_se.requested, 1) != 0) {
		return;
	}
	U_LOG_W("conversion host: %s (lParam 0x%llx%s): closing for update (exit 20)", what,
	        (unsigned long long)lParam, (lParam & ENDSESSION_CLOSEAPP) ? " ENDSESSION_CLOSEAPP" : "");
	HANDLE t = CreateThread(NULL, 0, backstop_thread, NULL, 0, NULL);
	if (t != NULL) {
		CloseHandle(t);
	}
	if (g_se.on_close != NULL) {
		g_se.on_close(g_se.ctx);
	}
}

static LRESULT CALLBACK
session_end_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_QUERYENDSESSION:
		// Always agree, at once. A Restart Manager close request
		// (ENDSESSION_CLOSEAPP) is acted on here, not on WM_ENDSESSION: a
		// provider dependency may own a non-pumping window that stalls RM's
		// query round (same reasoning as the service's tray window).
		if (lParam & ENDSESSION_CLOSEAPP) {
			request_close("WM_QUERYENDSESSION", lParam);
		}
		return TRUE;
	case WM_ENDSESSION:
		if (wParam) {
			request_close("WM_ENDSESSION", lParam);
		}
		return 0;
	case WM_CLOSE: request_close("WM_CLOSE", lParam); return 0;
	default: return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}

static DWORD WINAPI
session_end_thread(LPVOID p)
{
	(void)p;
	HINSTANCE hi = GetModuleHandleW(NULL);
	WNDCLASSEXW wc = {0};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = session_end_proc;
	wc.hInstance = hi;
	wc.lpszClassName = L"DisplayXRConversionHostSessionEnd";
	(void)RegisterClassExW(&wc);
	// Top-level (not HWND_MESSAGE: message-only windows receive no session
	// messages), never shown.
	g_se.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"DisplayXR Conversion Host", WS_POPUP, 0, 0,
	                            0, 0, NULL, NULL, hi, NULL);
	SetEvent(g_se.ready);
	if (g_se.hwnd == NULL) {
		U_LOG_W("conversion host: session-end window creation failed (%lu); Restart Manager can only force "
		        "it closed",
		        GetLastError());
		return 1;
	}
	MSG m;
	while (GetMessageW(&m, NULL, 0, 0) > 0) {
		TranslateMessage(&m);
		DispatchMessageW(&m);
	}
	return 0;
}

bool
conv_host_os_start_session_end_watch(void (*on_close)(void *ctx), void *ctx, uint32_t backstop_ms)
{
	g_se.on_close = on_close;
	g_se.ctx = ctx;
	g_se.backstop_ms = backstop_ms;
	g_se.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
	HANDLE t = CreateThread(NULL, 0, session_end_thread, NULL, 0, NULL);
	if (t == NULL) {
		return false;
	}
	CloseHandle(t);
	if (g_se.ready != NULL) {
		(void)WaitForSingleObject(g_se.ready, 2000);
		CloseHandle(g_se.ready);
		g_se.ready = NULL;
	}
	return g_se.hwnd != NULL;
}

void *
conv_host_os_session_end_hwnd(void)
{
	return g_se.hwnd;
}

bool
conv_host_os_watch_parent(uint64_t parent_pid)
{
	(void)parent_pid;
	return true; // KILL_ON_JOB_CLOSE + the channel breaking
}

uint64_t
conv_host_os_pid(void)
{
	return (uint64_t)GetCurrentProcessId();
}
