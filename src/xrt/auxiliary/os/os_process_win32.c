// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  os_process.h on Windows: job object, named pipe, DuplicateHandle,
 *         auto-reset events.
 * @ingroup aux_os
 */

#include "os/os_process.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Helpers.
 *
 */

static HANDLE
to_handle(uint64_t h)
{
	return (HANDLE)(uintptr_t)h;
}

static uint64_t
from_handle(HANDLE h)
{
	if (h == NULL || h == INVALID_HANDLE_VALUE) {
		return OS_NATIVE_HANDLE_INVALID;
	}
	return (uint64_t)(uintptr_t)h;
}

static bool
handle_valid(uint64_t h)
{
	return h != OS_NATIVE_HANDLE_INVALID && h != 0;
}

//! UTF-8 -> freshly allocated wide string (free()). NULL on failure.
static wchar_t *
widen(const char *s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	if (n <= 0) {
		return NULL;
	}
	wchar_t *w = (wchar_t *)calloc((size_t)n, sizeof(wchar_t));
	if (w == NULL) {
		return NULL;
	}
	if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
		free(w);
		return NULL;
	}
	return w;
}

/*!
 * Append @p arg to @p cmd quoted per the CommandLineToArgvW rules (backslashes
 * before a quote are doubled, the quote escaped, trailing backslashes doubled).
 * @p cmd has room for @p cap wide chars; *len is updated. false = overflow.
 */
static bool
append_quoted(wchar_t *cmd, size_t cap, size_t *len, const wchar_t *arg)
{
	size_t n = *len;
#define PUT(c)                                                                                                         \
	do {                                                                                                           \
		if (n + 1 >= cap) {                                                                                    \
			return false;                                                                                  \
		}                                                                                                      \
		cmd[n++] = (c);                                                                                        \
	} while (0)

	if (n > 0) {
		PUT(L' ');
	}
	PUT(L'"');
	for (const wchar_t *p = arg;; p++) {
		size_t backslashes = 0;
		while (*p == L'\\') {
			backslashes++;
			p++;
		}
		if (*p == L'\0') {
			for (size_t i = 0; i < backslashes * 2; i++) {
				PUT(L'\\');
			}
			break;
		}
		if (*p == L'"') {
			for (size_t i = 0; i < backslashes * 2 + 1; i++) {
				PUT(L'\\');
			}
			PUT(L'"');
		} else {
			for (size_t i = 0; i < backslashes; i++) {
				PUT(L'\\');
			}
			PUT(*p);
		}
	}
	PUT(L'"');
	cmd[n] = L'\0';
	*len = n;
	return true;
#undef PUT
}


/*
 *
 * Processes.
 *
 */

struct os_process
{
	HANDLE process;
	HANDLE job; //!< KILL_ON_JOB_CLOSE; NULL when not requested
	DWORD pid;
	bool exited;
	int32_t exit_code;
};

int
os_process_spawn(const struct os_process_spawn_info *info, struct os_process **out_proc)
{
	if (info == NULL || info->exe_path == NULL || out_proc == NULL || (info->arg_count > 0 && info->args == NULL)) {
		return OS_PROCESS_ERR_INVALID;
	}
	*out_proc = NULL;

	int ret = OS_PROCESS_ERR_FAILED;
	const size_t cap = 32768; // CreateProcessW's command line limit
	wchar_t *cmd = (wchar_t *)calloc(cap, sizeof(wchar_t));
	wchar_t *wexe = widen(info->exe_path);
	HANDLE job = NULL;
	HANDLE log = INVALID_HANDLE_VALUE;
	HANDLE nul = INVALID_HANDLE_VALUE;
	LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
	PROCESS_INFORMATION pi;
	memset(&pi, 0, sizeof(pi));

	if (cmd == NULL || wexe == NULL) {
		goto out;
	}

	size_t len = 0;
	if (!append_quoted(cmd, cap, &len, wexe)) {
		ret = OS_PROCESS_ERR_INVALID;
		goto out;
	}
	for (uint32_t i = 0; i < info->arg_count; i++) {
		wchar_t *w = widen(info->args[i] != NULL ? info->args[i] : "");
		bool ok = w != NULL && append_quoted(cmd, cap, &len, w);
		free(w);
		if (!ok) {
			ret = OS_PROCESS_ERR_INVALID;
			goto out;
		}
	}

	if (info->kill_on_parent_exit) {
		job = CreateJobObjectW(NULL, NULL);
		if (job == NULL) {
			goto out;
		}
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
		memset(&li, 0, sizeof(li));
		li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li))) {
			goto out;
		}
	}

	STARTUPINFOEXW si;
	memset(&si, 0, sizeof(si));
	si.StartupInfo.cb = sizeof(si);
	BOOL inherit = FALSE;
	DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;

	if (info->log_path != NULL) {
		// Only these two handles are inherited (an explicit handle list), so
		// nothing else the parent opened leaks into the child.
		SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
		wchar_t *wlog = widen(info->log_path);
		if (wlog == NULL) {
			goto out;
		}
		log = CreateFileW(wlog, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
		                  FILE_ATTRIBUTE_NORMAL, NULL);
		free(wlog);
		nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
		if (log == INVALID_HANDLE_VALUE || nul == INVALID_HANDLE_VALUE) {
			goto out;
		}
		SIZE_T attr_bytes = 0;
		InitializeProcThreadAttributeList(NULL, 1, 0, &attr_bytes);
		attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)calloc(1, attr_bytes);
		if (attrs == NULL || !InitializeProcThreadAttributeList(attrs, 1, 0, &attr_bytes)) {
			free(attrs);
			attrs = NULL;
			goto out;
		}
		HANDLE list[2] = {log, nul};
		if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, sizeof(list), NULL,
		                               NULL)) {
			goto out;
		}
		si.lpAttributeList = attrs;
		si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		si.StartupInfo.hStdInput = nul;
		si.StartupInfo.hStdOutput = log;
		si.StartupInfo.hStdError = log;
		inherit = TRUE;
		flags |= EXTENDED_STARTUPINFO_PRESENT;
	}

	if (!CreateProcessW(wexe, cmd, NULL, NULL, inherit, flags, NULL, NULL, &si.StartupInfo, &pi)) {
		goto out;
	}
	if (job != NULL && !AssignProcessToJobObject(job, pi.hProcess)) {
		TerminateProcess(pi.hProcess, 1);
		WaitForSingleObject(pi.hProcess, 5000);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		memset(&pi, 0, sizeof(pi));
		goto out;
	}
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);

	struct os_process *p = (struct os_process *)calloc(1, sizeof(*p));
	if (p == NULL) {
		TerminateProcess(pi.hProcess, 1);
		CloseHandle(pi.hProcess);
		goto out;
	}
	p->process = pi.hProcess;
	p->job = job;
	p->pid = pi.dwProcessId;
	job = NULL;
	*out_proc = p;
	ret = 0;

out:
	if (attrs != NULL) {
		DeleteProcThreadAttributeList(attrs);
		free(attrs);
	}
	if (log != INVALID_HANDLE_VALUE) {
		CloseHandle(log);
	}
	if (nul != INVALID_HANDLE_VALUE) {
		CloseHandle(nul);
	}
	if (job != NULL) {
		CloseHandle(job);
	}
	free(wexe);
	free(cmd);
	return ret;
}

long
os_process_pid(const struct os_process *proc)
{
	return proc != NULL ? (long)proc->pid : 0;
}

int
os_process_wait(struct os_process *proc, uint32_t timeout_ms, int32_t *out_exit_code)
{
	if (proc == NULL) {
		return OS_PROCESS_ERR_INVALID;
	}
	if (!proc->exited) {
		DWORD w = WaitForSingleObject(proc->process, timeout_ms == OS_PROCESS_WAIT_INFINITE ? INFINITE : timeout_ms);
		if (w == WAIT_TIMEOUT) {
			return 0;
		}
		if (w != WAIT_OBJECT_0) {
			return OS_PROCESS_ERR_FAILED;
		}
		DWORD code = 0;
		if (!GetExitCodeProcess(proc->process, &code)) {
			return OS_PROCESS_ERR_FAILED;
		}
		proc->exited = true;
		proc->exit_code = (int32_t)code;
	}
	if (out_exit_code != NULL) {
		*out_exit_code = proc->exit_code;
	}
	return 1;
}

int
os_process_terminate(struct os_process *proc, int32_t exit_code)
{
	if (proc == NULL) {
		return OS_PROCESS_ERR_INVALID;
	}
	if (proc->exited) {
		return 0;
	}
	if (!TerminateProcess(proc->process, (UINT)exit_code)) {
		// Already exiting is fine; anything else is a failure.
		if (WaitForSingleObject(proc->process, 0) != WAIT_OBJECT_0) {
			return OS_PROCESS_ERR_FAILED;
		}
	}
	return 0;
}

void
os_process_destroy(struct os_process **proc_ptr)
{
	if (proc_ptr == NULL || *proc_ptr == NULL) {
		return;
	}
	struct os_process *p = *proc_ptr;
	if (p->process != NULL) {
		CloseHandle(p->process);
	}
	if (p->job != NULL) {
		CloseHandle(p->job);
	}
	free(p);
	*proc_ptr = NULL;
}

int
os_process_dup_to_child(struct os_process *proc, uint64_t local, uint64_t *out_remote)
{
	if (proc == NULL || !handle_valid(local) || out_remote == NULL) {
		return OS_PROCESS_ERR_INVALID;
	}
	HANDLE remote = NULL;
	if (!DuplicateHandle(GetCurrentProcess(), to_handle(local), proc->process, &remote, 0, FALSE,
	                     DUPLICATE_SAME_ACCESS)) {
		return OS_PROCESS_ERR_FAILED;
	}
	*out_remote = (uint64_t)(uintptr_t)remote;
	return 0;
}

int
os_process_dup_from_child(struct os_process *proc, uint64_t remote, bool close_source, uint64_t *out_local)
{
	if (proc == NULL || !handle_valid(remote) || out_local == NULL) {
		return OS_PROCESS_ERR_INVALID;
	}
	HANDLE local = NULL;
	DWORD opts = DUPLICATE_SAME_ACCESS | (close_source ? DUPLICATE_CLOSE_SOURCE : 0);
	if (!DuplicateHandle(proc->process, to_handle(remote), GetCurrentProcess(), &local, 0, FALSE, opts)) {
		return OS_PROCESS_ERR_FAILED;
	}
	*out_local = from_handle(local);
	return 0;
}

void
os_native_handle_close(uint64_t handle)
{
	if (handle_valid(handle)) {
		CloseHandle(to_handle(handle));
	}
}


/*
 *
 * Child-side helpers.
 *
 */

int
os_process_watch_parent(long parent_pid, int32_t exit_code)
{
	// The job object (KILL_ON_JOB_CLOSE) kills the child with the parent.
	(void)parent_pid;
	(void)exit_code;
	return 0;
}

bool
os_process_is_elevated(void)
{
	HANDLE token = NULL;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		return false;
	}
	TOKEN_ELEVATION elevation;
	DWORD size = 0;
	bool elevated = false;
	if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
		elevated = elevation.TokenIsElevated != 0;
	}
	CloseHandle(token);
	return elevated;
}

long
os_process_current_pid(void)
{
	return (long)GetCurrentProcessId();
}

bool
os_named_mutex_held(const char *name)
{
	if (name == NULL) {
		return false;
	}
	wchar_t *w = widen(name);
	if (w == NULL) {
		return false;
	}
	HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, w);
	DWORD err = GetLastError();
	free(w);
	if (m == NULL) {
		// Missing = free. Present but not openable by us (an elevated
		// installer's DACL) = assume held: never respawn into an update.
		return err == ERROR_ACCESS_DENIED;
	}
	DWORD r = WaitForSingleObject(m, 0);
	bool held = r == WAIT_TIMEOUT;
	if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) {
		ReleaseMutex(m);
	}
	CloseHandle(m);
	return held;
}


/*
 *
 * Channel: a single-instance, overlapped, byte-mode named pipe.
 *
 */

struct os_channel
{
	HANDLE pipe;
	HANDLE read_event;
	HANDLE write_event;
	bool server;
};

static struct os_channel *
channel_alloc(HANDLE pipe, bool server)
{
	struct os_channel *c = (struct os_channel *)calloc(1, sizeof(*c));
	if (c == NULL) {
		return NULL;
	}
	c->pipe = pipe;
	c->server = server;
	c->read_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	c->write_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (c->read_event == NULL || c->write_event == NULL) {
		if (c->read_event != NULL) {
			CloseHandle(c->read_event);
		}
		if (c->write_event != NULL) {
			CloseHandle(c->write_event);
		}
		free(c);
		return NULL;
	}
	return c;
}

int
os_channel_create_for_child(const char *name_hint,
                            struct os_channel **out_parent,
                            char *out_token,
                            size_t token_size,
                            uint64_t *out_child_handle)
{
	static volatile LONG counter = 0;

	if (out_parent == NULL || out_token == NULL || token_size == 0) {
		return OS_PROCESS_ERR_INVALID;
	}
	*out_parent = NULL;
	if (out_child_handle != NULL) {
		*out_child_handle = OS_NATIVE_HANDLE_INVALID;
	}

	for (int attempt = 0; attempt < 8; attempt++) {
		LARGE_INTEGER qpc;
		QueryPerformanceCounter(&qpc);
		char name[256];
		int n = snprintf(name, sizeof(name), "\\\\.\\pipe\\%s-%lu-%ld-%08llx",
		                 name_hint != NULL ? name_hint : "displayxr-channel", (unsigned long)GetCurrentProcessId(),
		                 (long)InterlockedIncrement(&counter), (unsigned long long)(qpc.QuadPart & 0xFFFFFFFFull));
		if (n <= 0 || (size_t)n >= sizeof(name) || (size_t)n >= token_size) {
			return OS_PROCESS_ERR_INVALID;
		}
		wchar_t *w = widen(name);
		if (w == NULL) {
			return OS_PROCESS_ERR_FAILED;
		}
		HANDLE pipe = CreateNamedPipeW(
		    w, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
		    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, NULL);
		DWORD err = GetLastError();
		free(w);
		if (pipe == INVALID_HANDLE_VALUE) {
			if (err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY) {
				continue; // name taken: another suffix
			}
			return OS_PROCESS_ERR_FAILED;
		}
		struct os_channel *c = channel_alloc(pipe, true);
		if (c == NULL) {
			CloseHandle(pipe);
			return OS_PROCESS_ERR_FAILED;
		}
		memcpy(out_token, name, (size_t)n + 1);
		*out_parent = c;
		return 0;
	}
	return OS_PROCESS_ERR_FAILED;
}

int
os_channel_accept_child(struct os_channel *parent,
                        const struct os_process *child,
                        uint64_t child_handle,
                        uint32_t timeout_ms)
{
	(void)child_handle;
	if (parent == NULL || !parent->server || child == NULL) {
		return OS_PROCESS_ERR_INVALID;
	}

	OVERLAPPED ov;
	memset(&ov, 0, sizeof(ov));
	ov.hEvent = parent->read_event;
	ResetEvent(ov.hEvent);
	if (!ConnectNamedPipe(parent->pipe, &ov)) {
		DWORD err = GetLastError();
		if (err == ERROR_IO_PENDING) {
			// Wait for the connection, or for the child to die first.
			HANDLE waits[2] = {ov.hEvent, child->process};
			DWORD w = WaitForMultipleObjects(2, waits, FALSE,
			                                 timeout_ms == OS_PROCESS_WAIT_INFINITE ? INFINITE : timeout_ms);
			if (w != WAIT_OBJECT_0) {
				CancelIoEx(parent->pipe, &ov);
				DWORD dummy = 0;
				GetOverlappedResult(parent->pipe, &ov, &dummy, TRUE);
				return w == WAIT_TIMEOUT ? OS_PROCESS_ERR_TIMEOUT : OS_PROCESS_ERR_CLOSED;
			}
			DWORD dummy = 0;
			if (!GetOverlappedResult(parent->pipe, &ov, &dummy, FALSE)) {
				return OS_PROCESS_ERR_FAILED;
			}
		} else if (err != ERROR_PIPE_CONNECTED) {
			return OS_PROCESS_ERR_FAILED;
		}
	}

	// Peer credentials: the client must be the process we spawned.
	ULONG pid = 0;
	if (!GetNamedPipeClientProcessId(parent->pipe, &pid) || (DWORD)pid != child->pid) {
		DisconnectNamedPipe(parent->pipe);
		return OS_PROCESS_ERR_PEER;
	}
	return 0;
}

int
os_channel_open_child(const char *token, struct os_channel **out_channel)
{
	if (token == NULL || out_channel == NULL) {
		return OS_PROCESS_ERR_INVALID;
	}
	*out_channel = NULL;
	wchar_t *w = widen(token);
	if (w == NULL) {
		return OS_PROCESS_ERR_INVALID;
	}
	HANDLE pipe = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 4; attempt++) {
		pipe = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
		                   FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
		if (pipe != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) {
			break;
		}
		WaitNamedPipeW(w, 2000);
	}
	free(w);
	if (pipe == INVALID_HANDLE_VALUE) {
		return OS_PROCESS_ERR_FAILED;
	}
	struct os_channel *c = channel_alloc(pipe, false);
	if (c == NULL) {
		CloseHandle(pipe);
		return OS_PROCESS_ERR_FAILED;
	}
	*out_channel = c;
	return 0;
}

static int
map_pipe_error(DWORD err)
{
	switch (err) {
	case ERROR_BROKEN_PIPE:
	case ERROR_NO_DATA:
	case ERROR_PIPE_NOT_CONNECTED:
	case ERROR_OPERATION_ABORTED: return OS_PROCESS_ERR_CLOSED;
	default: return OS_PROCESS_ERR_FAILED;
	}
}

int
os_channel_send(struct os_channel *ch, const void *buf, size_t len, const uint64_t *handles, uint32_t handle_count)
{
	(void)handles;
	if (ch == NULL || (buf == NULL && len > 0)) {
		return OS_PROCESS_ERR_INVALID;
	}
	if (handle_count != 0) {
		return OS_PROCESS_ERR_UNSUPPORTED;
	}
	const uint8_t *p = (const uint8_t *)buf;
	while (len > 0) {
		DWORD chunk = len > 0x10000000u ? 0x10000000u : (DWORD)len;
		OVERLAPPED ov;
		memset(&ov, 0, sizeof(ov));
		ov.hEvent = ch->write_event;
		ResetEvent(ov.hEvent);
		DWORD written = 0;
		if (!WriteFile(ch->pipe, p, chunk, &written, &ov)) {
			DWORD err = GetLastError();
			if (err != ERROR_IO_PENDING) {
				return map_pipe_error(err);
			}
			if (!GetOverlappedResult(ch->pipe, &ov, &written, TRUE)) {
				return map_pipe_error(GetLastError());
			}
		} else if (!GetOverlappedResult(ch->pipe, &ov, &written, TRUE)) {
			return map_pipe_error(GetLastError());
		}
		if (written == 0) {
			return OS_PROCESS_ERR_CLOSED;
		}
		p += written;
		len -= written;
	}
	return 0;
}

/*! One overlapped read of up to @p len bytes. 1 = got *out_n > 0, 0 = timeout, < 0 = error. */
static int
read_some(struct os_channel *ch, uint8_t *p, size_t len, uint32_t timeout_ms, DWORD *out_n)
{
	DWORD chunk = len > 0x10000000u ? 0x10000000u : (DWORD)len;
	OVERLAPPED ov;
	memset(&ov, 0, sizeof(ov));
	ov.hEvent = ch->read_event;
	ResetEvent(ov.hEvent);
	DWORD n = 0;
	*out_n = 0;
	if (!ReadFile(ch->pipe, p, chunk, &n, &ov)) {
		DWORD err = GetLastError();
		if (err != ERROR_IO_PENDING) {
			return map_pipe_error(err);
		}
		DWORD w = WaitForSingleObject(ov.hEvent, timeout_ms == OS_PROCESS_WAIT_INFINITE ? INFINITE : timeout_ms);
		if (w == WAIT_TIMEOUT) {
			CancelIoEx(ch->pipe, &ov);
			// The read may have completed between the timeout and the cancel.
			if (!GetOverlappedResult(ch->pipe, &ov, &n, TRUE)) {
				DWORD e = GetLastError();
				if (e == ERROR_OPERATION_ABORTED) {
					return 0;
				}
				return map_pipe_error(e);
			}
		} else if (w != WAIT_OBJECT_0) {
			return OS_PROCESS_ERR_FAILED;
		} else if (!GetOverlappedResult(ch->pipe, &ov, &n, FALSE)) {
			return map_pipe_error(GetLastError());
		}
	} else if (!GetOverlappedResult(ch->pipe, &ov, &n, TRUE)) {
		return map_pipe_error(GetLastError());
	}
	if (n == 0) {
		return 0;
	}
	*out_n = n;
	return 1;
}

int
os_channel_recv(struct os_channel *ch,
                void *buf,
                size_t len,
                uint64_t *handles,
                uint32_t *inout_handle_count,
                uint32_t timeout_ms)
{
	(void)handles;
	if (inout_handle_count != NULL) {
		*inout_handle_count = 0;
	}
	if (ch == NULL || (buf == NULL && len > 0)) {
		return OS_PROCESS_ERR_INVALID;
	}
	uint8_t *p = (uint8_t *)buf;
	size_t got = 0;
	while (got < len) {
		DWORD n = 0;
		int r = read_some(ch, p + got, len - got, timeout_ms, &n);
		if (r < 0) {
			return r;
		}
		if (r == 0) {
			return got == 0 ? 0 : OS_PROCESS_ERR_TIMEOUT;
		}
		got += n;
	}
	return 1;
}

void
os_channel_destroy(struct os_channel **ch_ptr)
{
	if (ch_ptr == NULL || *ch_ptr == NULL) {
		return;
	}
	struct os_channel *c = *ch_ptr;
	if (c->pipe != INVALID_HANDLE_VALUE && c->pipe != NULL) {
		CancelIoEx(c->pipe, NULL);
		if (c->server) {
			DisconnectNamedPipe(c->pipe);
		}
		CloseHandle(c->pipe);
	}
	CloseHandle(c->read_event);
	CloseHandle(c->write_event);
	free(c);
	*ch_ptr = NULL;
}


/*
 *
 * Wake: an auto-reset event.
 *
 */

int
os_wake_create_pair(struct os_wake *local, uint64_t *out_remote_native)
{
	if (local == NULL || out_remote_native == NULL) {
		return OS_PROCESS_ERR_INVALID;
	}
	HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (ev == NULL) {
		local->native = OS_NATIVE_HANDLE_INVALID;
		*out_remote_native = OS_NATIVE_HANDLE_INVALID;
		return OS_PROCESS_ERR_FAILED;
	}
	local->native = from_handle(ev);
	*out_remote_native = local->native;
	return 0;
}

void
os_wake_from_native(struct os_wake *w, uint64_t native)
{
	w->native = native;
}

int
os_wake_signal(struct os_wake *w)
{
	if (w == NULL || !handle_valid(w->native)) {
		return OS_PROCESS_ERR_INVALID;
	}
	return SetEvent(to_handle(w->native)) ? 0 : OS_PROCESS_ERR_FAILED;
}

int
os_wake_wait(struct os_wake *w, uint32_t timeout_ms)
{
	if (w == NULL || !handle_valid(w->native)) {
		return OS_PROCESS_ERR_INVALID;
	}
	DWORD r = WaitForSingleObject(to_handle(w->native), timeout_ms == OS_PROCESS_WAIT_INFINITE ? INFINITE : timeout_ms);
	if (r == WAIT_OBJECT_0) {
		return 1;
	}
	if (r == WAIT_TIMEOUT) {
		return 0;
	}
	return OS_PROCESS_ERR_FAILED;
}

void
os_wake_destroy(struct os_wake *w)
{
	if (w == NULL) {
		return;
	}
	os_native_handle_close(w->native);
	w->native = OS_NATIVE_HANDLE_INVALID;
}
