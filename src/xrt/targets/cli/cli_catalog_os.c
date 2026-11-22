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
 * What an elevated engine says comes back through a named pipe this file
 * creates and reads (cli_catalog_os_log_open; the writer is
 * get_log_channel.h). The engine is given a token, never a path.
 *
 * Windows is the platform with an engine. The POSIX `run` exists so the unit
 * test can drive the front end with its fake engine on macOS and Linux;
 * @ref cli_catalog_os_locate_engine never finds one there, so the shipped CLI
 * never reaches it.
 *
 * @ingroup targets_cli
 */

#include "cli_catalog.h"

#include "get_log_channel.h"

#include "xrt/xrt_config_os.h"

#include <stdlib.h>
#include <string.h>

//! The most engine output this CLI keeps (a catalog listing is a few hundred KB at most).
#define MAX_CAPTURE (16u * 1024u * 1024u)
//! The most of an engine log this CLI keeps as text. Past it, only `result:` lines are kept.
#define MAX_LOG_BYTES (256u * 1024u)
//! The most `result:` lines kept past that bound (one per item; the engine takes 32 items).
#define MAX_LOG_RESULT_BYTES (64u * 1024u)

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

/*
 * What the engine sent through the log channel, assembled line by line. The
 * text is kept up to MAX_LOG_BYTES. Past that the messages are dropped, but a
 * `result:` line never is: it is how each item's outcome reaches this CLI.
 */
struct log_capture
{
	struct capture text;
	char line[1024];
	size_t line_len;
	size_t result_bytes; //!< `result:` lines kept past the bound
	bool truncated;
};

static void
log_capture_line(struct log_capture *lc, bool ended)
{
	static const char cut[] = "[dxr-get: more output than this CLI keeps; only result lines follow]\n";
	static const char prefix[] = "result: ";
	const size_t n = lc->line_len;
	lc->line_len = 0;
	if (n == 0 && !ended) {
		return;
	}
	if (lc->text.len + n + 1 <= MAX_LOG_BYTES) {
		capture_add(&lc->text, lc->line, n);
		if (ended) {
			capture_add(&lc->text, "\n", 1);
		}
		return;
	}
	if (!lc->truncated) {
		lc->truncated = true;
		capture_add(&lc->text, cut, sizeof(cut) - 1);
	}
	if (n >= sizeof(prefix) - 1 && memcmp(lc->line, prefix, sizeof(prefix) - 1) == 0 &&
	    lc->result_bytes + n + 1 <= MAX_LOG_RESULT_BYTES) {
		lc->result_bytes += n + 1;
		capture_add(&lc->text, lc->line, n);
		capture_add(&lc->text, "\n", 1);
	}
}

static void
log_capture_add(struct log_capture *lc, const char *data, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (data[i] == '\n') {
			log_capture_line(lc, true);
		} else if (data[i] == '\0') {
			continue;
		} else {
			if (lc->line_len == sizeof(lc->line)) {
				log_capture_line(lc, false); // an over-long line is kept in pieces
			}
			lc->line[lc->line_len++] = data[i];
		}
	}
}

//! The text so far (the caller `free()`s it; NULL when nothing was sent), and reset.
static char *
log_capture_take(struct log_capture *lc)
{
	if (lc->line_len > 0) {
		log_capture_line(lc, false);
	}
	char *text = lc->text.buf;
	memset(lc, 0, sizeof(*lc));
	return text;
}

static void
token_hex(const unsigned char *bytes, char token[DXR_LOG_TOKEN_LEN + 1])
{
	static const char hex[] = "0123456789abcdef";
	for (int i = 0; i < DXR_LOG_TOKEN_LEN / 2; i++) {
		token[i * 2] = hex[bytes[i] >> 4];
		token[i * 2 + 1] = hex[bytes[i] & 0xF];
	}
	token[DXR_LOG_TOKEN_LEN] = '\0';
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
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <bcrypt.h>

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

//! Take what the pipe holds now. Never blocks: a pipe with no bytes is left alone. True when it held some.
static bool
drain(HANDLE rd, struct capture *cap)
{
	bool any = false;
	for (;;) {
		DWORD avail = 0;
		if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) || avail == 0) {
			return any;
		}
		char chunk[16384];
		DWORD got = 0;
		const DWORD want = avail < sizeof(chunk) ? avail : (DWORD)sizeof(chunk);
		if (!ReadFile(rd, chunk, want, &got, NULL) || got == 0) {
			return any;
		}
		capture_add(cap, chunk, got);
		any = true;
	}
}

/*
 * The log channel: a named pipe THIS process creates and reads, which the
 * engine connects to by token (`--log-pipe`, get_log_channel.h). One at a
 * time. It is read while the engine runs (log_pump, from both run loops), so
 * the engine is never held up by a full pipe.
 */
static struct
{
	HANDLE pipe; //!< NULL: none
	char token[DXR_LOG_TOKEN_LEN + 1];
	struct log_capture got;
} g_log;

//! Take what the engine has sent so far. Never blocks. True when there was something.
static bool
log_pump(void)
{
	bool any = false;
	if (g_log.pipe == NULL) {
		return false;
	}
	for (;;) {
		DWORD avail = 0;
		// Fails while no engine is connected, and once it has gone and the pipe is empty.
		if (!PeekNamedPipe(g_log.pipe, NULL, 0, NULL, &avail, NULL) || avail == 0) {
			return any;
		}
		char chunk[16384];
		DWORD got = 0;
		const DWORD want = avail < sizeof(chunk) ? avail : (DWORD)sizeof(chunk);
		if (!ReadFile(g_log.pipe, chunk, want, &got, NULL) || got == 0) {
			return any;
		}
		log_capture_add(&g_log.got, chunk, got);
		any = true;
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
		// A roomy pipe: the engine is not held up between two reads of this loop.
		if (!CreatePipe(&rd, &wr, &sa, 256 * 1024)) {
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
		bool busy = rd != NULL && drain(rd, &cap);
		busy = log_pump() || busy;
		// While output is flowing, do not sleep between reads.
		if (WaitForSingleObject(pi.hProcess, busy ? 0 : 20) == WAIT_OBJECT_0) {
			if (rd != NULL) {
				(void)drain(rd, &cap); // what it wrote last
			}
			(void)log_pump();
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
	// The log channel is read while waiting, so the engine never waits on it.
	enum cli_catalog_run_status status = CLI_CATALOG_RUN_OK;
	const ULONGLONG deadline = GetTickCount64() + r->timeout_ms;
	for (;;) {
		const bool busy = log_pump();
		const DWORD w = WaitForSingleObject(sei.hProcess, busy ? 0 : 50);
		if (w == WAIT_OBJECT_0) {
			DWORD code = 0;
			if (GetExitCodeProcess(sei.hProcess, &code)) {
				r->exit_code = (int)code;
			} else {
				status = CLI_CATALOG_RUN_FAILED;
			}
			(void)log_pump();
			break;
		}
		if (w != WAIT_TIMEOUT) {
			status = CLI_CATALOG_RUN_FAILED;
			break;
		}
		if (GetTickCount64() >= deadline) {
			status = CLI_CATALOG_RUN_TIMEOUT;
			break;
		}
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

/*
 * The engine file, held open from its signature check on. The handle shares
 * reading only: until this process ends nobody can write to the file, delete
 * it or rename it (or rename a directory above it), so the file that is
 * started elevated is the file whose signature was checked.
 */
static HANDLE g_engine_pin = INVALID_HANDLE_VALUE;

static HANDLE
pin_file(const wchar_t *file)
{
	// Another reader that also denies sharing (a scanner) can hold it for a moment.
	for (int attempt = 0;; attempt++) {
		HANDLE h =
		    CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION || attempt >= 4) {
			return h;
		}
		Sleep(100);
	}
}

/*
 * The same check dxr-get makes of a downloaded installer (get_verify.c): a
 * valid Authenticode signature that chains to a trusted root, and the leaf
 * certificate's display name. No revocation fetch: the check must not wait
 * on the network, and it runs before a consent prompt that names the
 * publisher anyway.
 *
 * For the engine (@p path given) the file is opened first, without write or
 * delete sharing, the signature is read through that handle, and the handle
 * stays open (g_engine_pin). A file that cannot be opened that way is not
 * verified.
 */
bool
cli_catalog_os_signer(void *ud, const char *path, char *signer, size_t cap)
{
	(void)ud;
	if (cap > 0) {
		signer[0] = '\0';
	}
	wchar_t self[MAX_PATH];
	wchar_t *owned = NULL;
	const wchar_t *file = self;
	HANDLE pin = NULL;
	if (path != NULL) {
		owned = widen(path);
		file = owned;
		if (owned == NULL) {
			return false;
		}
		if (g_engine_pin != INVALID_HANDLE_VALUE) {
			CloseHandle(g_engine_pin);
			g_engine_pin = INVALID_HANDLE_VALUE;
		}
		pin = pin_file(owned);
		if (pin == INVALID_HANDLE_VALUE) {
			free(owned);
			return false;
		}
	} else {
		const DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
		if (n == 0 || n >= MAX_PATH) {
			return false;
		}
	}

	WINTRUST_FILE_INFO fi;
	memset(&fi, 0, sizeof(fi));
	fi.cbStruct = sizeof(fi);
	fi.pcwszFilePath = file;
	fi.hFile = pin; // NULL for this executable: WinVerifyTrust opens it by path
	WINTRUST_DATA wd;
	memset(&wd, 0, sizeof(wd));
	wd.cbStruct = sizeof(wd);
	wd.dwUIChoice = WTD_UI_NONE;
	wd.fdwRevocationChecks = WTD_REVOKE_NONE;
	wd.dwUnionChoice = WTD_CHOICE_FILE;
	wd.pFile = &fi;
	wd.dwStateAction = WTD_STATEACTION_VERIFY;
	wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
	GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
	bool ok = false;
	if (WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd) == ERROR_SUCCESS) {
		CRYPT_PROVIDER_DATA *pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
		CRYPT_PROVIDER_SGNR *sg = pd != NULL ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : NULL;
		CRYPT_PROVIDER_CERT *pc = sg != NULL ? WTHelperGetProvCertFromChain(sg, 0) : NULL;
		if (pc != NULL && pc->pCert != NULL) {
			wchar_t name[256];
			if (CertGetNameStringW(pc->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, name, 256) > 1) {
				ok = narrow_out(name, signer, cap);
			}
		}
	}
	wd.dwStateAction = WTD_STATEACTION_CLOSE;
	(void)WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
	free(owned);
	if (pin != NULL) {
		if (ok) {
			g_engine_pin = pin; // kept until this process ends
		} else {
			CloseHandle(pin);
		}
	}
	return ok;
}

/*
 * The log channel's reading end (see g_log). The pipe is created HERE, by
 * the process that will read it, under a random name; the engine is given
 * the token and connects. FILE_FLAG_FIRST_PIPE_INSTANCE: a pipe of that name
 * that already exists is a failure, never something to share. The default
 * security of a named pipe lets its creator, administrators and SYSTEM write
 * to it, which is who an elevated engine runs as.
 */
bool
cli_catalog_os_log_open(void *ud, char *token, size_t cap)
{
	(void)ud;
	if (cap < DXR_LOG_TOKEN_LEN + 1) {
		return false;
	}
	if (g_log.pipe != NULL) {
		CloseHandle(g_log.pipe);
		g_log.pipe = NULL;
		free(log_capture_take(&g_log.got));
	}
	unsigned char rnd[DXR_LOG_TOKEN_LEN / 2];
	if (BCryptGenRandom(NULL, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
		return false;
	}
	token_hex(rnd, g_log.token);
	wchar_t name[80];
	dxr_log_channel_name(g_log.token, name);
	HANDLE h = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
	                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0,
	                            64 * 1024, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}
	g_log.pipe = h;
	memcpy(token, g_log.token, DXR_LOG_TOKEN_LEN + 1);
	return true;
}

char *
cli_catalog_os_log_take(void *ud, const char *token)
{
	(void)ud;
	if (g_log.pipe == NULL || strcmp(token, g_log.token) != 0) {
		return NULL;
	}
	(void)log_pump();
	// Closing the reading end is all the cleanup there is: no file was made.
	// An engine that is still running (a timeout) finds its next write refused
	// and goes on without the channel.
	CloseHandle(g_log.pipe);
	g_log.pipe = NULL;
	return log_capture_take(&g_log.got);
}

/*
 * This process's arguments as UTF-8, read from the UTF-16 command line. The C
 * runtime's `argv` is in the ANSI code page, and everything here hands
 * strings to the engine as UTF-8.
 */
const char **
cli_catalog_os_utf8_args(int argc)
{
	int n = 0;
	wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
	if (w == NULL) {
		return NULL;
	}
	const char **out = NULL;
	if (n == argc) { // the same split as the runtime's, or the caller keeps its own
		out = (const char **)calloc((size_t)n + 1, sizeof(char *));
	}
	for (int i = 0; out != NULL && i < n; i++) {
		const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL, NULL);
		char *a = len > 0 ? (char *)calloc((size_t)len, 1) : NULL;
		if (a == NULL || WideCharToMultiByte(CP_UTF8, 0, w[i], -1, a, len, NULL, NULL) <= 0) {
			free(a);
			out = NULL; // what was converted is left to the end of the process
			break;
		}
		out[i] = a;
	}
	LocalFree(w);
	return out;
}

bool
cli_catalog_os_absolute_path(void *ud, const char *path, char *out, size_t cap)
{
	(void)ud;
	wchar_t *w = widen(path);
	wchar_t full[2048];
	bool ok = false;
	if (w != NULL) {
		const DWORD n = GetFullPathNameW(w, 2048, full, NULL);
		ok = n > 0 && n < 2048 && narrow_out(full, out, cap);
	}
	free(w);
	return ok;
}

#else // !XRT_OS_WINDOWS

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
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

/*
 * The log channel's reading end, for the unit test's stand-in engine: a FIFO
 * named by the token (get_log_channel.h). One at a time, read while the
 * engine runs.
 */
static struct
{
	int fd; //!< valid when `open` is set
	bool open;
	char token[DXR_LOG_TOKEN_LEN + 1];
	char path[1024];
	struct log_capture got;
} g_log;

static void
log_pump(void)
{
	if (!g_log.open) {
		return;
	}
	for (;;) {
		char chunk[16384];
		const ssize_t got = read(g_log.fd, chunk, sizeof(chunk));
		if (got <= 0) { // nothing now, or no engine connected
			return;
		}
		log_capture_add(&g_log.got, chunk, (size_t)got);
	}
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
			log_pump();
		} else {
			struct timespec nap = {0, 20 * 1000 * 1000};
			(void)nanosleep(&nap, NULL);
			log_pump();
		}
		const pid_t done = waitpid(pid, &wstatus, WNOHANG);
		if (done == pid) {
			if (fds[0] >= 0) {
				drain(fds[0], &cap);
			}
			log_pump();
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

bool
cli_catalog_os_signer(void *ud, const char *path, char *signer, size_t cap)
{
	(void)ud;
	(void)path;
	if (cap > 0) {
		signer[0] = '\0';
	}
	return false; // no engine, no elevated start, nothing to verify
}

bool
cli_catalog_os_log_open(void *ud, char *token, size_t cap)
{
	(void)ud;
	if (cap < DXR_LOG_TOKEN_LEN + 1) {
		return false;
	}
	if (g_log.open) {
		close(g_log.fd);
		(void)unlink(g_log.path);
		g_log.open = false;
		free(log_capture_take(&g_log.got));
	}
	unsigned char rnd[DXR_LOG_TOKEN_LEN / 2];
	const int ur = open("/dev/urandom", O_RDONLY);
	const bool got = ur >= 0 && read(ur, rnd, sizeof(rnd)) == (ssize_t)sizeof(rnd);
	if (ur >= 0) {
		close(ur);
	}
	if (!got) {
		return false;
	}
	token_hex(rnd, g_log.token);
	dxr_log_channel_name(g_log.token, g_log.path, sizeof(g_log.path));
	if (mkfifo(g_log.path, 0600) != 0) { // fails when the name exists
		return false;
	}
	g_log.fd = open(g_log.path, O_RDONLY | O_NONBLOCK);
	if (g_log.fd < 0) {
		(void)unlink(g_log.path);
		return false;
	}
	g_log.open = true;
	memcpy(token, g_log.token, DXR_LOG_TOKEN_LEN + 1);
	return true;
}

char *
cli_catalog_os_log_take(void *ud, const char *token)
{
	(void)ud;
	if (!g_log.open || strcmp(token, g_log.token) != 0) {
		return NULL;
	}
	log_pump();
	close(g_log.fd);
	(void)unlink(g_log.path);
	g_log.open = false;
	return log_capture_take(&g_log.got);
}

const char **
cli_catalog_os_utf8_args(int argc)
{
	(void)argc;
	return NULL; // argv is already what the user typed
}

bool
cli_catalog_os_absolute_path(void *ud, const char *path, char *out, size_t cap)
{
	(void)ud;
	if (path[0] == '/') {
		return (size_t)snprintf(out, cap, "%s", path) < cap;
	}
	char cwd[1024];
	if (getcwd(cwd, sizeof(cwd)) == NULL) {
		return false;
	}
	return (size_t)snprintf(out, cap, "%s/%s", cwd, path) < cap;
}

#endif
