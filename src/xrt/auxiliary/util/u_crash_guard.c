// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Exit / terminate tripwires for long-lived host processes (#950).
 * @ingroup aux_util
 */

#include "u_crash_guard.h"
#include "u_logging.h"

#include "xrt/xrt_config_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef XRT_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif


/*
 *
 * Stack logging (module+offset, no dbghelp).
 *
 */

#define CRASH_GUARD_MAX_FRAMES 48
#define CRASH_GUARD_MSG_CAP 4096

#ifdef XRT_OS_WINDOWS
static void
append_frame(char *buf, size_t cap, size_t *len, int idx, const void *addr)
{
	HMODULE mod = NULL;
	char name[MAX_PATH] = {0};
	const char *base = "?";
	uintptr_t off = (uintptr_t)addr;

	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       (LPCSTR)addr, &mod) &&
	    mod != NULL) {
		off = (uintptr_t)addr - (uintptr_t)mod;
		if (GetModuleFileNameA(mod, name, (DWORD)sizeof(name)) > 0) {
			base = strrchr(name, '\\');
			base = (base != NULL) ? base + 1 : name;
		} else {
			base = "?";
		}
	}

	if (*len < cap) {
		int n = snprintf(buf + *len, cap - *len, "\n    #%02d %s+0x%llx (%p)", idx, base,
		                 (unsigned long long)off, addr);
		if (n > 0) {
			*len += (size_t)n < cap - *len ? (size_t)n : cap - *len - 1;
		}
	}
}
#endif

void
u_crash_guard_log_stack(const char *tag, const char *reason)
{
	char buf[CRASH_GUARD_MSG_CAP];

#ifdef XRT_OS_WINDOWS
	size_t len = 0;
	void *frames[CRASH_GUARD_MAX_FRAMES];
	USHORT n = CaptureStackBackTrace(1, CRASH_GUARD_MAX_FRAMES, frames, NULL);
	int written = snprintf(buf, sizeof(buf),
	                       "%s %s (thread %lu, %u frames; symbolise offline: cdb -z <dump> or "
	                       "`ln <module>+<offset>` against the deployed PDBs)",
	                       tag, reason, (unsigned long)GetCurrentThreadId(), (unsigned)n);
	len = written > 0 ? (size_t)written : 0;
	for (USHORT i = 0; i < n; i++) {
		append_frame(buf, sizeof(buf), &len, (int)i, frames[i]);
	}
#else
	snprintf(buf, sizeof(buf), "%s %s (no stack capture on this platform)", tag, reason);
#endif
	buf[sizeof(buf) - 1] = '\0';
	U_LOG_E("%s", buf);
}


/*
 *
 * Structured-exception guard at thread entries.
 *
 */

#if defined(XRT_OS_WINDOWS) && defined(_MSC_VER)

struct guard_ctx
{
	const char *thread_name;
};

static LONG
crash_guard_filter(struct guard_ctx *ctx, EXCEPTION_POINTERS *ep)
{
	char reason[512];
	DWORD code = (ep != NULL && ep->ExceptionRecord != NULL) ? ep->ExceptionRecord->ExceptionCode : 0;
	void *addr = (ep != NULL && ep->ExceptionRecord != NULL) ? ep->ExceptionRecord->ExceptionAddress : NULL;
	const char *kind = "SEH exception";
	if (code == 0xE06D7363u) {
		kind = "unhandled C++ exception (would std::terminate)";
	} else if (code == EXCEPTION_ACCESS_VIOLATION) {
		kind = "access violation";
	} else if (code == EXCEPTION_STACK_OVERFLOW) {
		kind = "stack overflow";
	}

	// Name the faulting address in module+offset form up front — the captured
	// stack below starts inside the exception dispatcher, so this line is what
	// `ub`/`ln` wants first.
	HMODULE mod = NULL;
	char name[MAX_PATH] = {0};
	const char *base = "?";
	uintptr_t off = (uintptr_t)addr;
	if (addr != NULL &&
	    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       (LPCSTR)addr, &mod) &&
	    mod != NULL && GetModuleFileNameA(mod, name, (DWORD)sizeof(name)) > 0) {
		off = (uintptr_t)addr - (uintptr_t)mod;
		base = strrchr(name, '\\');
		base = (base != NULL) ? base + 1 : name;
	}

	snprintf(reason, sizeof(reason),
	         "thread '%s' died: %s code=0x%08lx at %s+0x%llx (%p); propagating unchanged (WER/terminate as before)",
	         ctx != NULL && ctx->thread_name != NULL ? ctx->thread_name : "?", kind, (unsigned long)code, base,
	         (unsigned long long)off, addr);
	u_crash_guard_log_stack("[TERMINATE]", reason);

	// Behaviour-preserving: we only observe. Continue the search so the
	// unhandled-exception path (WER dump / std::terminate) runs exactly as it
	// would without the guard.
	return EXCEPTION_CONTINUE_SEARCH;
}

void *
u_crash_guard_run(const char *thread_name, void *(*fn)(void *), void *arg)
{
	struct guard_ctx ctx = {thread_name};
	void *ret = NULL;
	__try {
		ret = fn(arg);
	} __except (crash_guard_filter(&ctx, GetExceptionInformation())) {
		// unreachable: the filter always continues the search
	}
	return ret;
}

#else /* !MSVC */

void *
u_crash_guard_run(const char *thread_name, void *(*fn)(void *), void *arg)
{
	(void)thread_name;
	return fn(arg);
}

#endif


/*
 *
 * atexit tripwire.
 *
 */

static volatile bool g_orderly_exit = false;
static volatile bool g_tripwire_installed = false;

void
u_crash_guard_mark_orderly_exit(void)
{
	g_orderly_exit = true;
}

static void
exit_tripwire(void)
{
	if (g_orderly_exit) {
		return;
	}
	// We are inside exit() on the calling thread: whoever called exit() is on
	// this stack. This is the record #943 lacked (an in-process DLL calling
	// exit() leaves the log with a clean banner and no cause).
	u_crash_guard_log_stack("[EXIT]",
	                        "unexpected process exit() — not on the orderly shutdown path; the caller "
	                        "is on this stack");
}

void
u_crash_guard_install_exit_tripwire(void)
{
	if (g_tripwire_installed) {
		return;
	}
	g_tripwire_installed = true;
	atexit(exit_tripwire);
}
