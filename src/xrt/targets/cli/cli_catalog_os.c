// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displayxr-cli catalog`: finding and running the install engine.
 *
 * Every wait here is bounded (docs/reference/workspace-stability.md): the
 * engine is polled against a deadline, its output pipe is only ever read
 * when it has bytes, and a child that outlives the deadline is reported, not
 * waited for. The pipe is never read to end-of-file, because a process the
 * engine starts (an installer, the service) can inherit its write end and
 * keep it open long after the engine exits.
 *
 * Windows is the platform with an engine. The POSIX `run` exists so the unit
 * test can drive the front end with its fake engine on macOS and Linux;
 * @ref cli_catalog_os_locate_engine never finds one there, so the shipped CLI
 * never reaches it.
 *
 * @ingroup targets_cli
 */

#include "cli_catalog.h"

#include "xrt/xrt_config_os.h"

#include <stdlib.h>
#include <string.h>

//! The most engine output this CLI keeps (a catalog listing is a few hundred KB at most).
#define MAX_CAPTURE (16u * 1024u * 1024u)

struct capture
{
	char *buf;
	size_t len;
	size_t cap;
};

static void
capture_add(struct capture *c, const char *data, size_t n)
{
	if (c->len + n + 1 > MAX_CAPTURE) {
		return; // dropped; the parse then fails and the verb reports unexpected output
	}
	if (c->len + n + 1 > c->cap) {
		size_t cap = c->cap != 0 ? c->cap * 2 : 8192;
		while (cap < c->len + n + 1) {
			cap *= 2;
		}
		char *nb = (char *)realloc(c->buf, cap);
		if (nb == NULL) {
			return;
		}
		c->buf = nb;
		c->cap = cap;
	}
	memcpy(c->buf + c->len, data, n);
	c->len += n;
	c->buf[c->len] = '\0';
}

#ifdef XRT_OS_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <io.h>
#include <wchar.h>

#define ENGINE_EXE L"dxr-get.exe"

static wchar_t *
widen(const char *s)
{
	const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	if (n <= 0) {
		return NULL;
	}
	wchar_t *w = (wchar_t *)calloc((size_t)n, sizeof(wchar_t));
	if (w != NULL && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
		free(w);
		return NULL;
	}
	return w;
}

/*!
 * The arguments as one command-line string. With @p exe it starts with the
 * quoted program name (what `CreateProcess` wants); without, it is the
 * parameter string `ShellExecuteEx` wants.
 */
static wchar_t *
command_line(const char *exe, const char *const *args, int n_args)
{
	size_t cap = 16 + (exe != NULL ? strlen(exe) * 2 + 3 : 0);
	for (int i = 0; i < n_args; i++) {
		cap += strlen(args[i]) * 2 + 3;
	}
	char *line = (char *)calloc(cap, 1);
	if (line == NULL) {
		return NULL;
	}
	size_t len = 0;
	bool ok = true;
	if (exe != NULL) {
		ok = cli_catalog_quote_arg(exe, line, cap);
		len = strlen(line);
	}
	for (int i = 0; ok && i < n_args; i++) {
		if (len > 0) {
			line[len++] = ' ';
		}
		ok = cli_catalog_quote_arg(args[i], line + len, cap - len);
		len += strlen(line + len);
	}
	wchar_t *w = ok ? widen(line) : NULL;
	free(line);
	return w;
}

//! Append @p src to @p dst (capacity @p cap, in characters). False, and @p dst unchanged, when it does not fit.
static bool
wappend(wchar_t *dst, size_t cap, const wchar_t *src)
{
	const size_t have = wcslen(dst), add = wcslen(src);
	if (have + add + 1 > cap) {
		return false;
	}
	memcpy(dst + have, src, (add + 1) * sizeof(wchar_t));
	return true;
}

static bool
file_exists_w(const wchar_t *path)
{
	const DWORD a = GetFileAttributesW(path);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool
narrow_out(const wchar_t *w, char *out, size_t cap)
{
	return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) > 0;
}

bool
cli_catalog_os_locate_engine(char *out, size_t cap, char *why, size_t why_cap)
{
	wchar_t path[MAX_PATH * 2];

	// Next to this executable: the runtime installer puts both in one directory.
	const DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
	if (n > 0 && n < MAX_PATH) {
		wchar_t *slash = wcsrchr(path, L'\\');
		if (slash != NULL) {
			slash[1] = L'\0';
			if (wappend(path, MAX_PATH * 2, ENGINE_EXE) && file_exists_w(path) &&
			    narrow_out(path, out, cap)) {
				return true;
			}
		}
	}

	// A CLI run from a build tree: the installed runtime's copy.
	HKEY key;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\DisplayXR\\Runtime", 0, KEY_READ | KEY_WOW64_64KEY, &key) ==
	    ERROR_SUCCESS) {
		DWORD type = 0;
		DWORD bytes = (DWORD)(MAX_PATH * sizeof(wchar_t));
		const LSTATUS st = RegQueryValueExW(key, L"InstallPath", NULL, &type, (LPBYTE)path, &bytes);
		RegCloseKey(key);
		if (st == ERROR_SUCCESS && type == REG_SZ && bytes >= sizeof(wchar_t)) {
			path[bytes / sizeof(wchar_t)] = L'\0';
			const size_t len = wcslen(path);
			// An empty value must not turn into a path relative to the current directory.
			const bool sep = len > 0 && (path[len - 1] == L'\\' || wappend(path, MAX_PATH * 2, L"\\"));
			if (sep && wappend(path, MAX_PATH * 2, ENGINE_EXE) && file_exists_w(path) &&
			    narrow_out(path, out, cap)) {
				return true;
			}
		}
	}

	(void)snprintf(why, why_cap,
	               "dxr-get.exe was not found next to displayxr-cli, nor in the runtime's install directory "
	               "(HKLM\\Software\\DisplayXR\\Runtime\\InstallPath). The runtime installer installs it: "
	               "install or repair the DisplayXR runtime. Nothing was listed or changed.");
	return false;
}

//! Take what the pipe holds now. Never blocks: a pipe with no bytes is left alone.
static void
drain(HANDLE rd, struct capture *cap)
{
	for (;;) {
		DWORD avail = 0;
		if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) || avail == 0) {
			return;
		}
		char chunk[4096];
		DWORD got = 0;
		const DWORD want = avail < sizeof(chunk) ? avail : (DWORD)sizeof(chunk);
		if (!ReadFile(rd, chunk, want, &got, NULL) || got == 0) {
			return;
		}
		capture_add(cap, chunk, got);
	}
}

enum cli_catalog_run_status
cli_catalog_os_run(void *ud, struct cli_catalog_run *r)
{
	(void)ud;
	r->out = NULL;
	r->exit_code = -1;
	wchar_t *exe = widen(r->exe);
	wchar_t *line = command_line(r->exe, r->args, r->n_args);
	if (exe == NULL || line == NULL) {
		free(exe);
		free(line);
		return CLI_CATALOG_RUN_FAILED;
	}
	for (wchar_t *p = exe; *p != L'\0'; p++) {
		if (*p == L'/') {
			*p = L'\\'; // CreateProcess takes the application name as written
		}
	}

	HANDLE rd = NULL, wr = NULL;
	if (r->capture_stdout) {
		SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
		if (!CreatePipe(&rd, &wr, &sa, 0)) {
			free(exe);
			free(line);
			return CLI_CATALOG_RUN_FAILED;
		}
		(void)SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	}

	(void)fflush(stdout);
	(void)fflush(stderr);
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	memset(&pi, 0, sizeof(pi));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = r->capture_stdout ? wr : GetStdHandle(STD_OUTPUT_HANDLE);
	si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	const BOOL started = CreateProcessW(exe, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
	free(exe);
	free(line);
	if (wr != NULL) {
		CloseHandle(wr); // the child holds its own copy
	}
	if (!started) {
		if (rd != NULL) {
			CloseHandle(rd);
		}
		return CLI_CATALOG_RUN_FAILED;
	}

	struct capture cap = {NULL, 0, 0};
	enum cli_catalog_run_status status = CLI_CATALOG_RUN_OK;
	const ULONGLONG deadline = GetTickCount64() + r->timeout_ms;
	for (;;) {
		if (rd != NULL) {
			drain(rd, &cap);
		}
		if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
			if (rd != NULL) {
				drain(rd, &cap); // what it wrote last
			}
			break;
		}
		if (GetTickCount64() >= deadline) {
			status = CLI_CATALOG_RUN_TIMEOUT;
			if (r->kill_on_timeout) {
				(void)TerminateProcess(pi.hProcess, 1);
			}
			break;
		}
	}
	if (status == CLI_CATALOG_RUN_OK) {
		DWORD code = 0;
		if (GetExitCodeProcess(pi.hProcess, &code)) {
			r->exit_code = (int)code;
		} else {
			status = CLI_CATALOG_RUN_FAILED;
		}
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	if (rd != NULL) {
		CloseHandle(rd);
	}
	if (r->capture_stdout && cap.buf == NULL) {
		cap.buf = (char *)calloc(1, 1);
	}
	r->out = cap.buf;
	return status;
}

enum cli_catalog_run_status
cli_catalog_os_run_elevated(void *ud, struct cli_catalog_run *r)
{
	(void)ud;
	r->out = NULL;
	r->exit_code = -1;
	wchar_t *exe = widen(r->exe);
	wchar_t *params = command_line(NULL, r->args, r->n_args);
	if (exe == NULL || params == NULL) {
		free(exe);
		free(params);
		return CLI_CATALOG_RUN_FAILED;
	}

	// "runas" on the ENGINE, never on this CLI: the consent prompt names
	// dxr-get.exe and its publisher, and this process keeps the rights it has.
	const HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	SHELLEXECUTEINFOW sei;
	memset(&sei, 0, sizeof(sei));
	sei.cbSize = sizeof(sei);
	sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
	sei.lpVerb = L"runas";
	sei.lpFile = exe;
	sei.lpParameters = params;
	sei.nShow = SW_SHOWNORMAL;
	const BOOL started = ShellExecuteExW(&sei);
	const DWORD start_err = started ? ERROR_SUCCESS : GetLastError();
	free(exe);
	free(params);
	if (SUCCEEDED(co)) {
		CoUninitialize();
	}
	if (!started) {
		return start_err == ERROR_CANCELLED ? CLI_CATALOG_RUN_DECLINED : CLI_CATALOG_RUN_FAILED;
	}
	if (sei.hProcess == NULL) {
		return CLI_CATALOG_RUN_FAILED;
	}

	// Bounded. A child that is still installing when the bound passes is
	// reported and left running, as dxr-get leaves a slow installer running.
	enum cli_catalog_run_status status = CLI_CATALOG_RUN_OK;
	const DWORD w = WaitForSingleObject(sei.hProcess, r->timeout_ms);
	if (w == WAIT_OBJECT_0) {
		DWORD code = 0;
		if (GetExitCodeProcess(sei.hProcess, &code)) {
			r->exit_code = (int)code;
		} else {
			status = CLI_CATALOG_RUN_FAILED;
		}
	} else {
		status = w == WAIT_TIMEOUT ? CLI_CATALOG_RUN_TIMEOUT : CLI_CATALOG_RUN_FAILED;
	}
	CloseHandle(sei.hProcess);
	return status;
}

bool
cli_catalog_os_is_interactive(void *ud)
{
	(void)ud;
	return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stderr)) != 0;
}

#else // !XRT_OS_WINDOWS

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

bool
cli_catalog_os_locate_engine(char *out, size_t cap, char *why, size_t why_cap)
{
	if (cap > 0) {
		out[0] = '\0';
	}
	(void)snprintf(why, why_cap,
	               "the install engine (dxr-get) exists on Windows only for now; macOS and Linux get one with "
	               "ADR-064 step 5. Nothing was listed or changed. Until then, install components from "
	               "their own packages.");
	return false;
}

static uint64_t
now_ms(void)
{
	struct timespec ts;
	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

//! Take what the pipe holds now. The descriptor is non-blocking.
static void
drain(int fd, struct capture *cap)
{
	for (;;) {
		char chunk[4096];
		const ssize_t got = read(fd, chunk, sizeof(chunk));
		if (got <= 0) {
			return;
		}
		capture_add(cap, chunk, (size_t)got);
	}
}

enum cli_catalog_run_status
cli_catalog_os_run(void *ud, struct cli_catalog_run *r)
{
	(void)ud;
	r->out = NULL;
	r->exit_code = -1;
	if (access(r->exe, X_OK) != 0) {
		return CLI_CATALOG_RUN_FAILED;
	}
	const char **argv = (const char **)calloc((size_t)r->n_args + 2, sizeof(char *));
	if (argv == NULL) {
		return CLI_CATALOG_RUN_FAILED;
	}
	argv[0] = r->exe;
	for (int i = 0; i < r->n_args; i++) {
		argv[i + 1] = r->args[i];
	}

	int fds[2] = {-1, -1};
	if (r->capture_stdout && pipe(fds) != 0) {
		free(argv);
		return CLI_CATALOG_RUN_FAILED;
	}
	(void)fflush(NULL);
	const pid_t pid = fork();
	if (pid < 0) {
		if (fds[0] >= 0) {
			close(fds[0]);
			close(fds[1]);
		}
		free(argv);
		return CLI_CATALOG_RUN_FAILED;
	}
	if (pid == 0) {
		if (fds[1] >= 0) {
			(void)dup2(fds[1], STDOUT_FILENO);
			close(fds[0]);
			close(fds[1]);
		}
		execv(r->exe, (char *const *)argv);
		_exit(127);
	}
	free(argv);
	if (fds[1] >= 0) {
		close(fds[1]);
		(void)fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
	}

	struct capture cap = {NULL, 0, 0};
	enum cli_catalog_run_status status = CLI_CATALOG_RUN_OK;
	const uint64_t deadline = now_ms() + r->timeout_ms;
	int wstatus = 0;
	for (;;) {
		if (fds[0] >= 0) {
			struct pollfd p = {fds[0], POLLIN, 0};
			(void)poll(&p, 1, 20);
			drain(fds[0], &cap);
		} else {
			struct timespec nap = {0, 20 * 1000 * 1000};
			(void)nanosleep(&nap, NULL);
		}
		const pid_t done = waitpid(pid, &wstatus, WNOHANG);
		if (done == pid) {
			if (fds[0] >= 0) {
				drain(fds[0], &cap);
			}
			break;
		}
		if (done < 0 && errno != EINTR) {
			status = CLI_CATALOG_RUN_FAILED;
			break;
		}
		if (now_ms() >= deadline) {
			status = CLI_CATALOG_RUN_TIMEOUT;
			if (r->kill_on_timeout) {
				(void)kill(pid, SIGKILL);
				(void)waitpid(pid, &wstatus, 0);
			}
			break;
		}
	}
	if (fds[0] >= 0) {
		close(fds[0]);
	}
	if (status == CLI_CATALOG_RUN_OK) {
		r->exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 128 + WTERMSIG(wstatus);
	}
	if (r->capture_stdout && cap.buf == NULL) {
		cap.buf = (char *)calloc(1, 1);
	}
	r->out = cap.buf;
	return status;
}

enum cli_catalog_run_status
cli_catalog_os_run_elevated(void *ud, struct cli_catalog_run *r)
{
	(void)ud;
	r->out = NULL;
	r->exit_code = -1;
	return CLI_CATALOG_RUN_UNSUPPORTED;
}

bool
cli_catalog_os_is_interactive(void *ud)
{
	(void)ud;
	return isatty(STDIN_FILENO) != 0 && isatty(STDERR_FILENO) != 0;
}

#endif
