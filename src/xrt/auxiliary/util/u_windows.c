// Copyright 2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Various helpers for doing Windows specific things.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 *
 * @ingroup aux_os
 */

#include "xrt/xrt_windows.h"

#include "util/u_windows.h"
#include "util/u_logging.h"

#include "assert.h"


/*
 *
 * Helper functions.
 *
 */

#define LOG_D(...) U_LOG_IFL_D(log_level, __VA_ARGS__)
#define LOG_I(...) U_LOG_IFL_I(log_level, __VA_ARGS__)
#define LOG_W(...) U_LOG_IFL_W(log_level, __VA_ARGS__)
#define LOG_E(...) U_LOG_IFL_E(log_level, __VA_ARGS__)

#define GET_LAST_ERROR_STR(BUF) (u_winerror(BUF, ARRAY_SIZE(BUF), GetLastError(), true))


static bool
check_privilege_on_process(HANDLE hProcess, LPCTSTR lpszPrivilege, LPBOOL pfResult)
{
	PRIVILEGE_SET ps;
	LUID luid;
	HANDLE hToken;
	BOOL bRet, bHas;
	char buf[512];


	bRet = LookupPrivilegeValue( //
	    NULL,                    //
	    lpszPrivilege,           //
	    &luid);                  //
	if (!bRet) {
		U_LOG_E("LookupPrivilegeValue: '%s'", GET_LAST_ERROR_STR(buf));
		return false;
	}

	bRet = OpenProcessToken( //
	    hProcess,            //
	    TOKEN_QUERY,         //
	    &hToken);            //
	if (!bRet) {
		U_LOG_E("OpenProcessToken: '%s'", GET_LAST_ERROR_STR(buf));
		return false;
	}

	ps.PrivilegeCount = 1;
	ps.Control = PRIVILEGE_SET_ALL_NECESSARY;
	ps.Privilege[0].Luid = luid;
	ps.Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;

	bRet = PrivilegeCheck( //
	    hToken,            //
	    &ps,               //
	    &bHas);            //

	CloseHandle(hToken); // Done with token now.

	if (!bRet) {
		U_LOG_E("PrivilegeCheck: '%s'", GET_LAST_ERROR_STR(buf));
		return false;
	}

	*pfResult = bHas;

	return true;
}

static bool
enable_privilege_on_process(HANDLE hProcess, LPCTSTR lpszPrivilege)
{
	TOKEN_PRIVILEGES tp;
	LUID luid;
	HANDLE hToken;
	BOOL bRet;
	char buf[512];

	bRet = LookupPrivilegeValue( //
	    NULL,                    //
	    lpszPrivilege,           //
	    &luid);                  //
	if (!bRet) {
		U_LOG_E("LookupPrivilegeValue: '%s'", GET_LAST_ERROR_STR(buf));
		return false;
	}

	bRet = OpenProcessToken(     //
	    hProcess,                //
	    TOKEN_ADJUST_PRIVILEGES, //
	    &hToken);                //
	if (!bRet) {
		U_LOG_E("OpenProcessToken: '%s'", GET_LAST_ERROR_STR(buf));
		return false;
	}

	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

	bRet = AdjustTokenPrivileges( //
	    hToken,                   //
	    FALSE,                    //
	    &tp,                      //
	    sizeof(TOKEN_PRIVILEGES), //
	    (PTOKEN_PRIVILEGES)NULL,  //
	    (PDWORD)NULL);            //

	CloseHandle(hToken); // Done with token now.

	if (!bRet) {
		U_LOG_E("AdjustTokenPrivileges: '%s'", GET_LAST_ERROR_STR(buf));
		return false;
	}

	if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
		U_LOG_D("AdjustTokenPrivileges return ok but we got:\n\t'%s'", GET_LAST_ERROR_STR(buf));
		return false;
	}

	return true;
}

bool
try_to_grant_privilege(enum u_logging_level log_level, HANDLE hProcess, LPCTSTR lpszPrivilege)
{
	BOOL bRet, bHas;

	if (check_privilege_on_process(hProcess, lpszPrivilege, &bHas)) {
		LOG_D("%s: %s", lpszPrivilege, bHas ? "true" : "false");
		if (bHas) {
			LOG_I("Already had privilege '%s'.", lpszPrivilege);
			return true;
		}
	}

	LOG_D("Trying to grant privilege '%s'.", lpszPrivilege);

	bRet = enable_privilege_on_process(hProcess, lpszPrivilege);

	if (check_privilege_on_process(hProcess, lpszPrivilege, &bHas)) {
		LOG_D("%s: %s", lpszPrivilege, bHas ? "true" : "false");
		if (bHas == TRUE) {
			LOG_I("Granted privilege '%s'.", lpszPrivilege);
			return true;
		}
	}

	LOG_I("Failed to grant privilege '%s'.", lpszPrivilege);

	return false;
}

static const char *
get_priority_string(DWORD dwPriorityClass)
{
	switch (dwPriorityClass) {
	case ABOVE_NORMAL_PRIORITY_CLASS: return "ABOVE_NORMAL_PRIORITY_CLASS";
	case BELOW_NORMAL_PRIORITY_CLASS: return "BELOW_NORMAL_PRIORITY_CLASS";
	case HIGH_PRIORITY_CLASS: return "HIGH_PRIORITY_CLASS";
	case IDLE_PRIORITY_CLASS: return "IDLE_PRIORITY_CLASS";
	case NORMAL_PRIORITY_CLASS: return "NORMAL_PRIORITY_CLASS";
	case PROCESS_MODE_BACKGROUND_BEGIN: return "PROCESS_MODE_BACKGROUND_BEGIN";
	case PROCESS_MODE_BACKGROUND_END: return "PROCESS_MODE_BACKGROUND_END";
	case REALTIME_PRIORITY_CLASS: return "REALTIME_PRIORITY_CLASS";
	default: return "Unknown";
	}
}

static bool
try_to_raise_priority(enum u_logging_level log_level, HANDLE hProcess)
{
	BOOL bRet;
	char buf[512];

	// Doesn't fail
	DWORD dwPriClassAtStart = GetPriorityClass(hProcess);

	if (dwPriClassAtStart == REALTIME_PRIORITY_CLASS) {
		LOG_I("Already have priority 'REALTIME_PRIORITY_CLASS'.");
		return true;
	}

	LOG_D("Trying to raise priority to 'REALTIME_PRIORITY_CLASS'.");

	bRet = SetPriorityClass(hProcess, REALTIME_PRIORITY_CLASS);
	if (bRet == FALSE) {
		LOG_E("SetPriorityClass: %s", GET_LAST_ERROR_STR(buf));
		return false;
	}

	// Doesn't fail
	DWORD dwPriClassNow = GetPriorityClass(hProcess);

	if (dwPriClassNow != dwPriClassAtStart) {
		LOG_I("Raised priority class to '%s'", get_priority_string(dwPriClassNow));
		return true;
	} else {
		LOG_W("Could not raise priority at all, is/was '%s'.", get_priority_string(dwPriClassNow));
		return false;
	}
}


/*
 *
 * 'Exported' functions.
 *
 */

const char *
u_winerror(char *s, size_t size, DWORD err, bool remove_end)
{
	DWORD dSize = (DWORD)size;
	assert(dSize == size);
	BOOL bRet;

	bRet = FormatMessageA(          //
	    FORMAT_MESSAGE_FROM_SYSTEM, //
	    NULL,                       //
	    err,                        //
	    LANG_SYSTEM_DEFAULT,        //
	    s,                          //
	    dSize,                      //
	    NULL);                      //
	if (!bRet) {
		s[0] = 0;
	}

	if (!remove_end) {
		return s;
	}

	// Remove newline and period from message.
	size = strnlen_s(s, size);
	for (size_t i = size; i-- > 0;) {
		switch (s[i]) {
		case '.':
		case '\n':
		case '\r': //
			s[i] = '\0';
			continue;
		default: break;
		}
		break;
	}

	return s;
}

bool
u_win_grant_inc_base_priorty_base_privileges(enum u_logging_level log_level)
{
	// Always succeeds
	HANDLE hProcess = GetCurrentProcess();

	// Do not need to free hProcess.
	return try_to_grant_privilege(log_level, hProcess, SE_INC_BASE_PRIORITY_NAME);
}

bool
u_win_raise_cpu_priority(enum u_logging_level log_level)
{
	// Always succeeds
	HANDLE hProcess = GetCurrentProcess();

	// Do not need to free hProcess.
	return try_to_raise_priority(log_level, hProcess);
}

void
u_win_try_privilege_or_priority_from_args(enum u_logging_level log_level, int argc, char *argv[])
{
	if (argc > 1 && strcmp(argv[1], "nothing") == 0) {
		LOG_I("Not trying privileges or priority");
	} else if (argc > 1 && strcmp(argv[1], "priv") == 0) {
		LOG_I("Setting privileges");
		u_win_grant_inc_base_priorty_base_privileges(log_level);
	} else if (argc > 1 && strcmp(argv[1], "prio") == 0) {
		LOG_I("Setting priority");
		u_win_raise_cpu_priority(log_level);
	} else {
		LOG_I("Setting both privilege and priority");
		u_win_grant_inc_base_priorty_base_privileges(log_level);
		u_win_raise_cpu_priority(log_level);
	}
}


/*
 *
 * DPI awareness (#1201).
 *
 * A DPI-unaware process is handed VIRTUALISED coordinates by every GDI entry
 * point, so on a 4K panel at 150% scaling it "sees" a 2560x1440 display. That
 * is how `displayxr-cli` came to report — and its self-test to ASSERT —
 * the wrong panel resolution on a scaled box while DPI-aware apps on the same
 * machine at the same moment read the panel correctly. Awareness is a process
 * property, so it is the EXE's to declare; every DisplayXR executable now
 * declares the same one (per-monitor v2), by manifest first and by this call
 * as the backstop.
 *
 * The APIs are resolved dynamically: SetProcessDpiAwarenessContext is Win10
 * 1607+, SetProcessDpiAwareness is 8.1+, and SetProcessDPIAware is Vista+ —
 * we try them in that order and stop at the first that takes.
 *
 */

// DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is the opaque handle value -4.
#define U_WIN_DPI_CTX_PER_MONITOR_AWARE_V2 ((HANDLE)(intptr_t)-4)

typedef BOOL(WINAPI *pfn_set_process_dpi_awareness_context)(HANDLE);
typedef HANDLE(WINAPI *pfn_get_thread_dpi_awareness_context)(void);
typedef int(WINAPI *pfn_get_awareness_from_dpi_awareness_context)(HANDLE);
typedef BOOL(WINAPI *pfn_set_process_dpi_aware)(void);
typedef HRESULT(WINAPI *pfn_set_process_dpi_awareness)(int);

enum u_win_dpi_awareness
u_win_get_process_dpi_awareness(void)
{
	HMODULE user32 = GetModuleHandleA("user32.dll");
	if (user32 == NULL) {
		return U_WIN_DPI_UNKNOWN;
	}

	// GetThreadDpiAwarenessContext reports the process default for a thread
	// that never overrode it, which is what every DisplayXR EXE's main
	// thread is. Both entry points are Win10 1607+, same as the setter.
	pfn_get_thread_dpi_awareness_context get_ctx =
	    (pfn_get_thread_dpi_awareness_context)GetProcAddress(user32, "GetThreadDpiAwarenessContext");
	pfn_get_awareness_from_dpi_awareness_context to_awareness =
	    (pfn_get_awareness_from_dpi_awareness_context)GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext");
	if (get_ctx == NULL || to_awareness == NULL) {
		return U_WIN_DPI_UNKNOWN;
	}

	// DPI_AWARENESS: INVALID = -1, UNAWARE = 0, SYSTEM_AWARE = 1,
	// PER_MONITOR_AWARE = 2 (v2 reports as PER_MONITOR_AWARE too).
	switch (to_awareness(get_ctx())) {
	case 0: return U_WIN_DPI_UNAWARE;
	case 1: return U_WIN_DPI_SYSTEM_AWARE;
	case 2: return U_WIN_DPI_PER_MONITOR_AWARE;
	default: return U_WIN_DPI_UNKNOWN;
	}
}

const char *
u_win_dpi_awareness_to_string(enum u_win_dpi_awareness awareness)
{
	switch (awareness) {
	case U_WIN_DPI_UNAWARE: return "unaware";
	case U_WIN_DPI_SYSTEM_AWARE: return "system-aware";
	case U_WIN_DPI_PER_MONITOR_AWARE: return "per-monitor-aware";
	default: return "unknown";
	}
}

bool
u_win_make_process_dpi_aware(bool *out_was_already_aware)
{
	bool already = u_win_get_process_dpi_awareness() == U_WIN_DPI_PER_MONITOR_AWARE;
	if (out_was_already_aware != NULL) {
		*out_was_already_aware = already;
	}
	if (already) {
		// The manifest did the job before a single instruction of ours ran.
		return true;
	}

	HMODULE user32 = GetModuleHandleA("user32.dll");
	if (user32 != NULL) {
		pfn_set_process_dpi_awareness_context set_ctx =
		    (pfn_set_process_dpi_awareness_context)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
		if (set_ctx != NULL && set_ctx(U_WIN_DPI_CTX_PER_MONITOR_AWARE_V2)) {
			return true;
		}
	}

	// Win8.1 fallback. PROCESS_PER_MONITOR_DPI_AWARE = 2.
	HMODULE shcore = LoadLibraryA("shcore.dll");
	if (shcore != NULL) {
		pfn_set_process_dpi_awareness set_awareness =
		    (pfn_set_process_dpi_awareness)GetProcAddress(shcore, "SetProcessDpiAwareness");
		bool ok = set_awareness != NULL && SUCCEEDED(set_awareness(2));
		FreeLibrary(shcore);
		if (ok) {
			return true;
		}
	}

	// Last resort: system-aware. Better than virtualised coordinates, but it
	// is NOT per-monitor, so report the truth rather than claim success.
	if (user32 != NULL) {
		pfn_set_process_dpi_aware set_aware =
		    (pfn_set_process_dpi_aware)GetProcAddress(user32, "SetProcessDPIAware");
		if (set_aware != NULL) {
			set_aware();
		}
	}

	return u_win_get_process_dpi_awareness() == U_WIN_DPI_PER_MONITOR_AWARE;
}
