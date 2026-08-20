// Copyright 2020-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Just the client connection setup/teardown bits.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup ipc_client
 */

#include "os/os_threading.h"
#include "xrt/xrt_results.h"
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "xrt/xrt_instance.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_config_os.h"
#include "xrt/xrt_config_android.h"

#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_file.h"
#include "util/u_debug.h"
#include "util/u_git_tag.h"
#include "util/u_system_helpers.h"

#include "shared/ipc_utils.h"
#include "shared/ipc_protocol.h"
#include "client/ipc_client_connection.h"

#include "ipc_client_generated.h"


#include <stdio.h>
#include <stdlib.h>
#if !defined(XRT_OS_WINDOWS)
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#else
#include <shellapi.h> // For ShellExecuteExA fallback in service auto-launch
#endif
#include <limits.h>

#ifdef XRT_GRAPHICS_BUFFER_HANDLE_IS_AHARDWAREBUFFER
#include "android/android_ahardwarebuffer_allocator.h"
#endif

#ifdef XRT_OS_ANDROID
#include "android/ipc_client_android.h"
#endif // XRT_OS_ANDROID

DEBUG_GET_ONCE_BOOL_OPTION(ipc_ignore_version, "IPC_IGNORE_VERSION", false)

#ifdef XRT_OS_ANDROID

#include <pthread.h>

/*
 *
 * #1056: adopting an already-connected socket instead of dialling.
 *
 * The normal Android connect is Java: `org.freedesktop.monado.ipc.Client`
 * (loaded out of the RUNTIME apk with `loadClassFromRuntimeApk`) binds
 * `MonadoService` over AIDL, makes a socketpair and hands one end to the
 * service. That needs a `Context`, cross-apk class loading and package
 * visibility, which is a poor fit for an embedder whose renderer/GPU process is
 * not the process that owns the app's Java world — Chromium being the motivating
 * case (`displayxr-browser` on Android, epic #1031).
 *
 * So: let a process that ALREADY holds a connected socket end skip the Java path
 * entirely. The privileged/browser-side process does the Java connect once and
 * ships the fd down (Mojo `PlatformHandle`, a `ParcelFileDescriptor` over its own
 * binder, `SCM_RIGHTS`, or the child-launch descriptor table); the adopting
 * process publishes it with one of:
 *
 *   - `ipc_client_connection_adopt_fd(fd)` before `xrCreateInstance` — for an
 *     embedder that receives the fd at runtime, and
 *   - `DXR_IPC_FD=<n>` in the environment — for a process that is handed the fd
 *     in its descriptor table at launch (Chromium's `FileDescriptorInfo` /
 *     `base::GlobalDescriptors` shape), and for test harnesses.
 *
 * The adopted fd is consumed ONCE, by the next connection setup; the caller keeps
 * ownership of what it passed (we dup, exactly like the Java path does).
 *
 * NOT a security boundary: an fd is a capability, and whoever can call this is
 * already inside the process. Server-side identity is unchanged (see
 * `ipc_server_peer_creds.c`) — and note SO_PEERCRED on a socketpair reports the
 * process that CREATED the pair, so an adopted connection is attributed to the
 * creator, not the adopter (#1056; documented in the extension spec).
 */

static pthread_mutex_t s_adopt_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_adopted_fd = -1;

/*!
 * Is @p fd something we are willing to talk the IPC protocol over?
 *
 * Cheap, local, and deliberately strict: a wrong fd here would otherwise fail
 * much later as a corrupt protocol stream. We require an open, connected,
 * AF_UNIX, SOCK_STREAM/SOCK_SEQPACKET socket.
 */
static bool
ipc_client_fd_is_plausible(int fd, const char **out_why)
{
	const char *why = NULL;
	int type = 0;
	int domain = 0;
	socklen_t len = 0;
	struct sockaddr_storage peer;
	socklen_t peer_len = sizeof(peer);

	if (fd < 0) {
		why = "negative fd";
		goto bad;
	}
	if (fcntl(fd, F_GETFD) < 0) {
		why = "not an open descriptor";
		goto bad;
	}

	len = sizeof(type);
	if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) != 0) {
		why = "not a socket";
		goto bad;
	}
	if (type != SOCK_STREAM && type != SOCK_SEQPACKET) {
		why = "socket is not stream/seqpacket";
		goto bad;
	}

	len = sizeof(domain);
	if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &len) == 0 && domain != AF_UNIX) {
		why = "socket is not AF_UNIX";
		goto bad;
	}

	// A socketpair end is "connected" even though it was never bound, so
	// getpeername() succeeding (with an empty sun_path) is the check that the
	// peer has not already gone away.
	if (getpeername(fd, (struct sockaddr *)&peer, &peer_len) != 0) {
		why = "socket is not connected";
		goto bad;
	}

	return true;

bad:
	if (out_why != NULL) {
		*out_why = why;
	}
	return false;
}

void
ipc_client_connection_adopt_fd(int fd)
{
	const char *why = NULL;

	if (!ipc_client_fd_is_plausible(fd, &why)) {
		U_LOG_E("ipc_client_connection_adopt_fd(%d): rejected — %s (#1056).", fd, why);
		return;
	}

	// Dup now: the caller owns what it passed, and the value must stay valid
	// even if the caller closes its copy before xrCreateInstance.
	int dup_fd = dup(fd);
	if (dup_fd < 0) {
		U_LOG_E("ipc_client_connection_adopt_fd(%d): dup failed, errno %d (#1056).", fd, errno);
		return;
	}
	// Never leak the connection into a child of the adopting process.
	(void)fcntl(dup_fd, F_SETFD, FD_CLOEXEC);

	pthread_mutex_lock(&s_adopt_mutex);
	int previous = s_adopted_fd;
	s_adopted_fd = dup_fd;
	pthread_mutex_unlock(&s_adopt_mutex);

	if (previous >= 0) {
		U_LOG_W("ipc_client_connection_adopt_fd: replacing a previously adopted fd %d (#1056).", previous);
		close(previous);
	}
	U_LOG_W("ipc_client_connection_adopt_fd: adopted fd %d (dup of %d) — the Java connect will be skipped (#1056).",
	        dup_fd, fd);
}

bool
ipc_client_connection_has_adopted_fd(void)
{
	// Deliberately does NOT validate or dup: this runs on the hybrid-mode
	// decision path (#1031), before any connection exists, and its only job is
	// to answer "did somebody hand this process a socket?". A bogus fd is
	// rejected later, by ipc_client_take_adopted_fd(), where the failure has a
	// connection to fail. Cheap enough to call unconditionally.
	pthread_mutex_lock(&s_adopt_mutex);
	bool have = s_adopted_fd >= 0;
	pthread_mutex_unlock(&s_adopt_mutex);

	if (have) {
		return true;
	}

	const char *env = getenv("DXR_IPC_FD");
	return env != NULL && env[0] != '\0';
}

/*!
 * Take the adopted fd, if there is one, else -1. Consumes it.
 *
 * The `DXR_IPC_FD` environment fallback is resolved here rather than at adopt
 * time so that a process launched with the fd in its descriptor table needs no
 * code at all before `xrCreateInstance`.
 */
static int
ipc_client_take_adopted_fd(void)
{
	pthread_mutex_lock(&s_adopt_mutex);
	int fd = s_adopted_fd;
	s_adopted_fd = -1;
	pthread_mutex_unlock(&s_adopt_mutex);

	if (fd >= 0) {
		return fd;
	}

	const char *env = getenv("DXR_IPC_FD");
	if (env == NULL || env[0] == '\0') {
		return -1;
	}

	char *end = NULL;
	long parsed = strtol(env, &end, 10);
	if (end == env || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
		U_LOG_E("DXR_IPC_FD='%s' is not a descriptor number — ignoring (#1056).", env);
		return -1;
	}

	const char *why = NULL;
	if (!ipc_client_fd_is_plausible((int)parsed, &why)) {
		U_LOG_E("DXR_IPC_FD=%ld: rejected — %s (#1056).", parsed, why);
		return -1;
	}

	int dup_fd = dup((int)parsed);
	if (dup_fd < 0) {
		U_LOG_E("DXR_IPC_FD=%ld: dup failed, errno %d (#1056).", parsed, errno);
		return -1;
	}
	(void)fcntl(dup_fd, F_SETFD, FD_CLOEXEC);
	U_LOG_W("DXR_IPC_FD=%ld adopted as fd %d — the Java connect will be skipped (#1056).", parsed, dup_fd);
	return dup_fd;
}

static bool
ipc_client_socket_connect(struct ipc_connection *ipc_c, struct _JavaVM *vm, void *context)
{
	// #1056: an already-connected socket short-circuits the whole Java path —
	// no Context, no cross-apk class load, no bindService, no package
	// visibility. ipc_c->ica stays NULL, which fini already tolerates.
	int adopted = ipc_client_take_adopted_fd();
	if (adopted >= 0) {
		IPC_INFO(ipc_c, "Adopted an already-connected service socket (fd %d) (#1056).", adopted);
		ipc_c->imc.ipc_handle = adopted;
		ipc_c->adopted = true;
		return true;
	}

	ipc_c->ica = ipc_client_android_create(vm, context);

	if (ipc_c->ica == NULL) {
		IPC_ERROR(ipc_c, "Client create error!");
		return false;
	}

	int socket = ipc_client_android_blocking_connect(ipc_c->ica);
	if (socket < 0) {
		IPC_ERROR(ipc_c, "Service Connect error!");
		return false;
	}
	// The ownership belongs to the Java object. Dup because the fd will be
	// closed when client destroy.
	socket = dup(socket);
	if (socket < 0) {
		IPC_ERROR(ipc_c, "Failed to dup fd with error %d!", errno);
		return false;
	}

	// Set socket.
	ipc_c->imc.ipc_handle = socket;

	return true;
}

#elif defined(XRT_OS_WINDOWS)

/*
 *
 * browser#103: adopting an already-connected pipe HANDLE instead of dialling.
 *
 * The Windows twin of the Android `DXR_IPC_FD` / `ipc_client_connection_adopt_fd`
 * mechanism above (#1056), and deliberately the SAME contract rather than a
 * second one: same one-shot consumption, same "we duplicate, the caller keeps
 * ownership of what it passed", same precedence (an adopted endpoint wins over a
 * normal connect), same "not a security boundary" framing.
 *
 * The motivating case is identical too. Chromium's GPU process runs a
 * `USER_LIMITED` restricted token whose restricted-SID list matches neither ACE
 * on the service pipe's SD, so `CreateFileA` on the pipe returns ACCESS_DENIED
 * (browser#103 experiment E0). The browser process — which is NOT sandboxed and
 * opens that pipe today — brokers a fresh endpoint with
 * @ref ipc_client_connection_export and ships the raw HANDLE to the GPU over its
 * own mojo transport (mojo duplicates it into the target itself; no hand-rolled
 * OpenProcess/DuplicateHandle). The GPU publishes it with one of:
 *
 *   - `ipc_client_connection_adopt_handle(h)` before `xrCreateInstance` — for an
 *     embedder that receives the HANDLE at runtime, and
 *   - `DXR_IPC_HANDLE=<decimal>` in the environment — for a process handed the
 *     HANDLE at launch (inherited or `UpdateProcThreadAttribute`-injected), and
 *     for test harnesses.
 *
 * The BROKER MUST NOT HANDSHAKE. `ipc_client_connection_init` is
 * connect -> setup_shm -> check_git_tag -> describe_client, and all three of
 * those have to run in the ADOPTING process so it gets its own shmem handle and
 * is peer-identified as itself. @ref ipc_client_connection_export therefore only
 * connects.
 *
 * Unlike the POSIX socketpair case, mis-attribution here is FATAL, not cosmetic:
 * `GetNamedPipeClientProcessId` reports the OPENER, and the server duplicates
 * shmem / woven-texture / fence handles into that process. Hence RC-1 — the
 * adopting client declares its own pid as the duplication target and peer
 * identity in the first exchange, before any handle crosses. See
 * `ipc_server_peer_creds.c`.
 */

//! Adopted-but-not-yet-consumed pipe endpoint. NULL = none. Published and taken
//! with Interlocked*, so no static initializer / lock is needed (and MSVC C11
//! <stdatomic.h> is off the table per the portability rules in CLAUDE.md).
static HANDLE volatile s_adopted_handle = NULL;

/*!
 * Is @p h something we are willing to talk the IPC protocol over?
 *
 * Cheap, local, and deliberately strict — the twin of
 * ipc_client_fd_is_plausible(). A wrong HANDLE here would otherwise fail much
 * later as a corrupt protocol stream. GetNamedPipeInfo() succeeding proves it is
 * a pipe handle at all; the PIPE_SERVER_END bit being CLEAR proves it is the
 * client end (adopting a listening server end would deadlock the service).
 */
static bool
ipc_client_handle_is_plausible(HANDLE h, const char **out_why)
{
	const char *why = NULL;
	DWORD flags = 0;

	if (h == NULL || h == INVALID_HANDLE_VALUE) {
		why = "null handle";
		goto bad;
	}
	if (!GetNamedPipeInfo(h, &flags, NULL, NULL, NULL)) {
		why = "not a named-pipe handle";
		goto bad;
	}
	if ((flags & PIPE_SERVER_END) != 0) {
		why = "handle is the SERVER end of the pipe";
		goto bad;
	}

	return true;

bad:
	if (out_why != NULL) {
		*out_why = why;
	}
	return false;
}

//! Duplicate into this process so the value stays valid even if the caller
//! closes its copy before xrCreateInstance. Mirrors the dup() in the fd path.
static HANDLE
ipc_client_dup_own_handle(HANDLE h)
{
	HANDLE dup = NULL;
	HANDLE self = GetCurrentProcess();
	if (!DuplicateHandle(self, h, self, &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
		return NULL;
	}
	return dup;
}

void
ipc_client_connection_adopt_handle(void *pipe_handle)
{
	HANDLE h = (HANDLE)pipe_handle;
	const char *why = NULL;

	if (!ipc_client_handle_is_plausible(h, &why)) {
		U_LOG_E("ipc_client_connection_adopt_handle(%p): rejected — %s (browser#103).", h, why);
		return;
	}

	HANDLE dup = ipc_client_dup_own_handle(h);
	if (dup == NULL) {
		DWORD err = GetLastError();
		U_LOG_E("ipc_client_connection_adopt_handle(%p): DuplicateHandle failed: %lu (browser#103).", h, err);
		return;
	}

	HANDLE previous = InterlockedExchangePointer((PVOID volatile *)&s_adopted_handle, dup);
	if (previous != NULL) {
		U_LOG_W("ipc_client_connection_adopt_handle: replacing a previously adopted handle %p (browser#103).",
		        previous);
		CloseHandle(previous);
	}
	U_LOG_W(
	    "ipc_client_connection_adopt_handle: adopted handle %p (duplicate of %p) — the pipe connect will be "
	    "skipped (browser#103).",
	    dup, h);
}

bool
ipc_client_connection_has_adopted_handle(void)
{
	// A peek, not a take — deliberately does NOT validate or duplicate. Twin of
	// ipc_client_connection_has_adopted_fd().
	if (InterlockedCompareExchangePointer((PVOID volatile *)&s_adopted_handle, NULL, NULL) != NULL) {
		return true;
	}

	const char *env = getenv("DXR_IPC_HANDLE");
	return env != NULL && env[0] != '\0';
}

/*!
 * Take the adopted pipe endpoint, if there is one, else NULL. Consumes it.
 *
 * The `DXR_IPC_HANDLE` environment fallback is resolved here rather than at
 * adopt time so that a process handed the HANDLE at launch needs no code at all
 * before `xrCreateInstance` — same reasoning as `DXR_IPC_FD`.
 */
static HANDLE
ipc_client_take_adopted_handle(void)
{
	HANDLE h = InterlockedExchangePointer((PVOID volatile *)&s_adopted_handle, NULL);
	if (h != NULL) {
		return h;
	}

	const char *env = getenv("DXR_IPC_HANDLE");
	if (env == NULL || env[0] == '\0') {
		return NULL;
	}

	char *end = NULL;
	unsigned long long parsed = strtoull(env, &end, 10);
	if (end == env || *end != '\0' || parsed == 0) {
		U_LOG_E("DXR_IPC_HANDLE='%s' is not a handle value — ignoring (browser#103).", env);
		return NULL;
	}

	HANDLE raw = (HANDLE)(uintptr_t)parsed;
	const char *why = NULL;
	if (!ipc_client_handle_is_plausible(raw, &why)) {
		U_LOG_E("DXR_IPC_HANDLE=%llu: rejected — %s (browser#103).", parsed, why);
		return NULL;
	}

	HANDLE dup = ipc_client_dup_own_handle(raw);
	if (dup == NULL) {
		U_LOG_E("DXR_IPC_HANDLE=%llu: DuplicateHandle failed: %lu (browser#103).", parsed, GetLastError());
		return NULL;
	}
	U_LOG_W("DXR_IPC_HANDLE=%llu adopted as handle %p — the pipe connect will be skipped (browser#103).", parsed,
	        dup);
	return dup;
}

// Total time we are willing to keep retrying a busy pipe before giving up, and
// the per-attempt WaitNamedPipe timeout used between retries.
#define IPC_CONNECT_BUSY_TOTAL_MS 5000
#define IPC_CONNECT_BUSY_WAIT_MS 500

// #1058 / browser#92: how long we keep retrying ERROR_FILE_NOT_FOUND while a
// live service instance is detectable (i.e. the pipe is missing only because
// that instance has not posted it yet), and the poll step in between.
#define IPC_CONNECT_STARTING_TOTAL_MS 5000
#define IPC_CONNECT_STARTING_STEP_MS 250

//! The service's single-instance mutex (see targets/service/main.c, #975). Held
//! for the process lifetime, so "can be opened" == "a service process is alive
//! in this session". Local\ namespace: same-session only, which is exactly the
//! scope of the pipe.
#define IPC_SERVICE_SINGLETON_NAME L"Local\\DisplayXR.Service.Singleton"

/*!
 * Is a displayxr-service process alive right now?
 *
 * Best-effort and deliberately conservative: a sandboxed client (AppContainer
 * Chrome) may be denied the open even when the service IS up, in which case we
 * report false and the caller keeps its pre-#1058 behavior. False negatives
 * only cost us the retry window; there are no false positives that matter.
 */
static bool
ipc_service_instance_alive(void)
{
	HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, IPC_SERVICE_SINGLETON_NAME);
	if (m == NULL) {
		return false;
	}
	CloseHandle(m);
	return true;
}

/*!
 * Open the service comm pipe, retrying on ERROR_PIPE_BUSY — and, while a
 * service instance is alive, on ERROR_FILE_NOT_FOUND.
 *
 * The server mainloop only posts the next listening pipe instance *after* it
 * finishes handling the previous connection, so when several clients connect at
 * once (e.g. a workspace launching multiple apps) some race into the gap and
 * get ERROR_PIPE_BUSY (231). The canonical Win32 client dance for that is to
 * WaitNamedPipe() for an instance to free up and retry, bounded by a deadline,
 * instead of failing xrCreateInstance outright. See issue #603.
 *
 * #1058 / browser#92: ERROR_FILE_NOT_FOUND (2) is no longer only "server not
 * running". Since #1002 the service RELAUNCHES ITSELF on device-lost, so a
 * client connecting during those couple of seconds finds no pipe even though a
 * service is coming up. That fell through to the auto-launch fallback, whose
 * spawned instance immediately exits on the #975 singleton guard — and the wait
 * loop there only waits on the process IT spawned, so xrCreateInstance failed
 * (-2) while a perfectly good service arrived a second later. Retry instead,
 * but ONLY while the singleton mutex says an instance is actually alive: with no
 * service running at all this returns on the first attempt exactly as before, so
 * the cold-start auto-launch path pays nothing.
 *
 * On failure the original GetLastError() is preserved so callers can still tell
 * "server not running" (ERROR_FILE_NOT_FOUND, drives the auto-launch fallback)
 * apart from "server up but saturated" (ERROR_PIPE_BUSY).
 */
static HANDLE
ipc_open_pipe_busy_retry(struct ipc_connection *ipc_c, const char *pipe_name)
{
	DWORD waited_ms = 0;
	DWORD starting_waited_ms = 0;
	for (int attempt = 1;; attempt++) {
		HANDLE pipe_inst =
		    CreateFileA(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
		if (pipe_inst != INVALID_HANDLE_VALUE) {
			if (waited_ms > 0 || starting_waited_ms > 0) {
				// Leave a breadcrumb that the retry path recovered a connect
				// that would previously have failed (#603 busy, #1058 starting).
				IPC_INFO(ipc_c, "Pipe %s was unavailable, connected after ~%u ms (attempt %d)",
				         pipe_name, waited_ms + starting_waited_ms, attempt);
			}
			return pipe_inst;
		}

		DWORD err = GetLastError();

		// #1058: the pipe is missing but a service instance is alive — it is
		// mid-start (or mid-restart) and has not posted the pipe yet. Poll.
		if (err == ERROR_FILE_NOT_FOUND && starting_waited_ms < IPC_CONNECT_STARTING_TOTAL_MS &&
		    ipc_service_instance_alive()) {
			if (starting_waited_ms == 0) {
				IPC_INFO(ipc_c,
				         "Pipe %s not posted yet but a service instance is alive (starting or "
				         "relaunching) — waiting up to %u ms",
				         pipe_name, (unsigned)IPC_CONNECT_STARTING_TOTAL_MS);
			}
			Sleep(IPC_CONNECT_STARTING_STEP_MS);
			starting_waited_ms += IPC_CONNECT_STARTING_STEP_MS;
			continue;
		}

		if (err != ERROR_PIPE_BUSY) {
			// Server-down or a genuine error: hand back with the error intact.
			SetLastError(err);
			return INVALID_HANDLE_VALUE;
		}

		if (waited_ms >= IPC_CONNECT_BUSY_TOTAL_MS) {
			IPC_ERROR(ipc_c, "Pipe %s still busy after %u ms, giving up", pipe_name, waited_ms);
			SetLastError(ERROR_PIPE_BUSY);
			return INVALID_HANDLE_VALUE;
		}

		// Block until an instance frees up, then retry. WaitNamedPipe returning
		// FALSE (its own timeout) is fine — we just loop until the deadline.
		WaitNamedPipeA(pipe_name, IPC_CONNECT_BUSY_WAIT_MS);
		waited_ms += IPC_CONNECT_BUSY_WAIT_MS;
	}
}

#if defined(NO_XRT_SERVICE_LAUNCH) || !defined(XRT_SERVICE_EXECUTABLE)
static HANDLE
ipc_connect_pipe(struct ipc_connection *ipc_c, const char *pipe_name)
{
	HANDLE pipe_inst = ipc_open_pipe_busy_retry(ipc_c, pipe_name);
	if (pipe_inst == INVALID_HANDLE_VALUE) {
		DWORD err = GetLastError();
		IPC_ERROR(ipc_c, "Connect to %s failed: %d %s", pipe_name, err, ipc_winerror(err));
	}
	return pipe_inst;
}
#else
// N.B. quality of life fallback to try launch the XRT_SERVICE_EXECUTABLE if pipe is not found
static HANDLE
ipc_connect_pipe(struct ipc_connection *ipc_c, const char *pipe_name)
{
	HANDLE pipe_inst = ipc_open_pipe_busy_retry(ipc_c, pipe_name);
	if (pipe_inst != INVALID_HANDLE_VALUE) {
		return pipe_inst;
	}
	DWORD err = GetLastError();
	IPC_ERROR(ipc_c, "Connect to %s failed: %d %s", pipe_name, err, ipc_winerror(err));
	if (err != ERROR_FILE_NOT_FOUND) {
		return INVALID_HANDLE_VALUE;
	}
	IPC_INFO(ipc_c, "Trying to launch " XRT_SERVICE_EXECUTABLE "...");
	HMODULE hmod;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCSTR)ipc_connect_pipe, &hmod)) {
		IPC_ERROR(ipc_c, "GetModuleHandleExA failed: %d %s", err, ipc_winerror(err));
		return INVALID_HANDLE_VALUE;
	}
	char service_path[MAX_PATH];
	if (!GetModuleFileNameA(hmod, service_path, sizeof(service_path))) {
		IPC_ERROR(ipc_c, "GetModuleFileNameA failed: %d %s", err, ipc_winerror(err));
		return INVALID_HANDLE_VALUE;
	}
	char *p = strrchr(service_path, '\\');
	if (!p) {
		IPC_ERROR(ipc_c, "failed to parse the path %s", service_path);
		return INVALID_HANDLE_VALUE;
	}
	strcpy(p + 1, XRT_SERVICE_EXECUTABLE);
	STARTUPINFOA si = {.cb = sizeof(si)};
	PROCESS_INFORMATION pi;
	if (!CreateProcessA(NULL, service_path, NULL, NULL, false, 0, NULL, NULL, &si, &pi)) {
		char first_path[MAX_PATH];
		strcpy(first_path, service_path);
		*p = 0;
		p = strrchr(service_path, '\\');
		if (!p) {
			err = GetLastError();
			IPC_INFO(ipc_c, XRT_SERVICE_EXECUTABLE " not found in %s: %d %s", service_path, err,
			         ipc_winerror(err));
			return INVALID_HANDLE_VALUE;
		}
		strcpy(p + 1, "service\\" XRT_SERVICE_EXECUTABLE);
		if (!CreateProcessA(NULL, service_path, NULL, NULL, false, 0, NULL, NULL, &si, &pi)) {
			err = GetLastError();
			IPC_INFO(ipc_c,
			         "CreateProcessA failed for both %s and %s: %d %s. "
			         "Trying ShellExecuteEx fallback (for AppContainer/sandboxed processes)...",
			         first_path, service_path, err, ipc_winerror(err));

			// Fallback: ShellExecuteEx works from more sandbox levels than CreateProcessA.
			// AppContainer processes (e.g. Chrome WebXR) may not have permission to
			// create child processes directly, but ShellExecuteEx can delegate to the Windows shell.
			SHELLEXECUTEINFOA sei = {0};
			sei.cbSize = sizeof(sei);
			sei.lpFile = first_path;
			sei.nShow = SW_HIDE;
			sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
			if (!ShellExecuteExA(&sei)) {
				// Also try the service\ subdirectory path
				sei.lpFile = service_path;
				if (!ShellExecuteExA(&sei)) {
					err = GetLastError();
					IPC_ERROR(ipc_c,
					          "All service launch methods failed (CreateProcessA and ShellExecuteEx). "
					          "Please start displayxr-service.exe manually or install it as a startup task. "
					          "Last error: %d %s",
					          err, ipc_winerror(err));
					return INVALID_HANDLE_VALUE;
				}
			}
			// ShellExecuteEx succeeded — fill in pi.hProcess for the wait loop below
			pi.hProcess = sei.hProcess;
			pi.hThread = NULL;
		}
	}
	IPC_INFO(ipc_c, "Launched %s (pid %d)... Waiting for %s...", service_path, pi.dwProcessId, pipe_name);
	if (pi.hThread) {
		CloseHandle(pi.hThread);
	}
	for (int i = 0;; i++) {
		pipe_inst = CreateFileA(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
		if (pipe_inst != INVALID_HANDLE_VALUE) {
			IPC_INFO(ipc_c, "Connected to %s after %d msec on try %d!", pipe_name, i * 100, i + 1);
			break;
		}
		err = GetLastError();
		// Keep waiting while the freshly-launched service is still spinning up
		// its pipe (FILE_NOT_FOUND) or is momentarily saturated (PIPE_BUSY).
		if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PIPE_BUSY) {
			IPC_ERROR(ipc_c, "Connect to %s failed: %d %s", pipe_name, err, ipc_winerror(err));
			break;
		}
		// #1058 / browser#92: our child exiting is NOT proof that no service is
		// coming. It exits immediately on the #975 singleton guard whenever
		// another instance owns it — which is precisely the case we land in when
		// the service is mid-relaunch (#1002 device-lost). Give the pipe a
		// bounded grace window after the child dies instead of failing
		// xrCreateInstance the instant it does.
		if (WaitForSingleObject(pi.hProcess, 100) != WAIT_TIMEOUT) {
			if (i * 100 >= IPC_CONNECT_STARTING_TOTAL_MS) {
				IPC_ERROR(ipc_c, "Connect to %s failed: %d %s (launched service exited)", pipe_name,
				          err, ipc_winerror(err));
				break;
			}
			// The wait above returned immediately (process gone), so pace the
			// poll ourselves rather than spinning.
			Sleep(100);
		}
	}
	CloseHandle(pi.hProcess);
	return pipe_inst;
}
#endif

// Use a fixed global pipe name for Windows.
// This is required for AppContainer apps (like Chrome WebXR) which have virtualized
// temp directories - using temp path would cause client and server to use different pipe names.
#define IPC_WINDOWS_PIPE_NAME "\\\\.\\pipe\\displayxr\\displayxr_comp_ipc"

//! Message mode is a property of the handle, and a duplicate shares the file
//! object, so this is idempotent — both the broker and the adopter set it.
static bool
ipc_client_set_pipe_message_mode(struct ipc_connection *ipc_c, HANDLE pipe_inst)
{
	DWORD mode = PIPE_READMODE_MESSAGE | PIPE_WAIT;
	if (!SetNamedPipeHandleState(pipe_inst, &mode, NULL, NULL)) {
		DWORD err = GetLastError();
		IPC_ERROR(ipc_c, "SetNamedPipeHandleState(PIPE_READMODE_MESSAGE | PIPE_WAIT) failed: %d %s", err,
		          ipc_winerror(err));
		return false;
	}
	return true;
}

static bool
ipc_client_socket_connect(struct ipc_connection *ipc_c)
{
	// browser#103: an already-connected endpoint short-circuits the whole
	// CreateFileA path — which is the point, since the sandboxed peer that needs
	// this cannot open the pipe at all. Exact analogue of the Android arm above.
	HANDLE adopted = ipc_client_take_adopted_handle();
	if (adopted != NULL) {
		IPC_INFO(ipc_c, "Adopted an already-connected service pipe (handle %p) (browser#103).", adopted);
		if (!ipc_client_set_pipe_message_mode(ipc_c, adopted)) {
			CloseHandle(adopted);
			return false;
		}
		ipc_c->imc.ipc_handle = adopted;
		ipc_c->adopted = true;
		return true;
	}

	HANDLE pipe_inst = ipc_connect_pipe(ipc_c, IPC_WINDOWS_PIPE_NAME);
	if (pipe_inst == INVALID_HANDLE_VALUE) {
		return false;
	}
	if (!ipc_client_set_pipe_message_mode(ipc_c, pipe_inst)) {
		return false;
	}

	// Set socket.
	ipc_c->imc.ipc_handle = pipe_inst;

	return true;
}

#else

/*!
 * Dial the service socket and hand back the connected fd. No handshake — that
 * is the caller's job, and browser#103's broker deliberately never does it.
 */
static int
ipc_client_posix_dial(struct ipc_connection *ipc_c)
{
#ifdef SOCK_CLOEXEC
	// Make sure the socket is not inherited by child processes. For one, when there is an fd to the socket
	// in the child process closing the client connection (or killing the connected process)
	// may not be seen in the server as client disconnection.
	const int flags = SOCK_CLOEXEC;
#else
	const int flags = 0;
#endif
	struct sockaddr_un addr;
	int ret;


	// create our IPC socket

	ret = socket(PF_UNIX, SOCK_STREAM | flags, 0);
	if (ret < 0) {
		IPC_ERROR(ipc_c, "Socket Create Error!");
		return -1;
	}

	int socket = ret;

	char sock_file[PATH_MAX];

	ssize_t size = u_file_get_path_in_runtime_dir(XRT_IPC_MSG_SOCK_FILENAME, sock_file, PATH_MAX);
	if (size == -1) {
		IPC_ERROR(ipc_c, "Could not get socket file name");
		close(socket);
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, sock_file);

	ret = connect(socket, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		IPC_ERROR(ipc_c, "Failed to connect to socket %s: %s!", sock_file, strerror(errno));
		close(socket);
		return -1;
	}

	return socket;
}

static bool
ipc_client_socket_connect(struct ipc_connection *ipc_c)
{
	int socket = ipc_client_posix_dial(ipc_c);
	if (socket < 0) {
		return false;
	}

	// Set socket.
	ipc_c->imc.ipc_handle = socket;

	return true;
}

#endif


/*!
 * browser#103 RC-1: tell the service which process this connection really
 * belongs to, before any handle crosses.
 *
 * Sent ONLY on an adopted connection — a normally-dialled client's wire traffic
 * is byte-for-byte what it was, and the service's behaviour for it is unchanged.
 *
 * WHAT WE DECLARE IS ALWAYS OURSELVES, and that is the whole trick. An adopting
 * process cannot know whether it also happens to be the process that opened the
 * transport: it was handed a connected endpoint, not a provenance. But it does
 * not need to. Its own pid is unconditionally the correct answer to both
 * questions the server is asking — which process must the shared memory, woven
 * texture and fence be duplicated into (this one: it is the process running the
 * client library and importing them), and whose identity should gates run
 * against (this one: it is the process making the calls). If the adopter turns
 * out to be the opener too, the declaration is a no-op that names the value the
 * server already had. So "adopt-time init always declares self" is correct
 * regardless of who opened the pipe, and needs no extra channel to tell the two
 * cases apart.
 *
 * A refusal is not fatal (the server answers accepted=0 rather than an error):
 * the connection continues with opener attribution, i.e. exactly today's
 * behaviour. On Windows that means the sandboxed peer will fail to import
 * handles, which is a loud, well-logged failure — much better than a connection
 * that dies at xrCreateInstance for a reason the caller cannot classify.
 */
static void
ipc_client_declare_peer(struct ipc_connection *ipc_c)
{
	struct ipc_peer_declaration decl = {0};
#ifdef XRT_OS_WINDOWS
	decl.target_pid = (int64_t)GetCurrentProcessId();
#else
	decl.target_pid = (int64_t)getpid();
#endif

	uint32_t accepted = 0;
	xrt_result_t xret = ipc_call_instance_declare_peer(ipc_c, &decl, &accepted);
	if (xret != XRT_SUCCESS) {
		IPC_WARN(ipc_c,
		         "Peer declaration call failed (xret %d) — continuing with the service's own view of "
		         "this connection (browser#103).",
		         xret);
		return;
	}
	if (accepted == 0) {
		IPC_WARN(ipc_c,
		         "The service REFUSED our peer declaration (pid %lld); handles will be duplicated to whoever "
		         "opened this connection. See the service log for the reason (browser#103).",
		         (long long)decl.target_pid);
		return;
	}
	IPC_INFO(ipc_c, "Adopted connection declared to the service as pid %lld (browser#103).",
	         (long long)decl.target_pid);
}

static xrt_result_t
ipc_client_setup_shm(struct ipc_connection *ipc_c)
{
	/*
	 * Get our shared memory area from the server.
	 */

	xrt_result_t xret = ipc_call_instance_get_shm_fd(ipc_c, &ipc_c->ism_handle, 1);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ipc_c, "Failed to retrieve shm fd!");
		return xret;
	}

	/*
	 * Now map it.
	 */

	const size_t size = sizeof(struct ipc_shared_memory);

#ifdef XRT_OS_WINDOWS
	DWORD access = FILE_MAP_READ | FILE_MAP_WRITE;

	ipc_c->ism = MapViewOfFile(ipc_c->ism_handle, access, 0, 0, size);
#else
	const int flags = MAP_SHARED;
	const int access = PROT_READ | PROT_WRITE;

	ipc_c->ism = mmap(NULL, size, access, flags, ipc_c->ism_handle, 0);
#endif

	if (ipc_c->ism == NULL) {
		IPC_ERROR(ipc_c, "Failed to mmap shm!");
		return XRT_ERROR_IPC_FAILURE;
	}

	return XRT_SUCCESS;
}

static xrt_result_t
ipc_client_check_git_tag(struct ipc_connection *ipc_c)
{
	// Does the git tags match?
	if (strncmp(u_git_tag, ipc_c->ism->u_git_tag, IPC_VERSION_NAME_LEN) == 0) {
		return XRT_SUCCESS;
	}

	IPC_ERROR(ipc_c, "DisplayXR client library version %s does not match service version %s", u_git_tag,
	          ipc_c->ism->u_git_tag);

	if (!debug_get_bool_option_ipc_ignore_version()) {
		IPC_ERROR(ipc_c, "Set IPC_IGNORE_VERSION=1 to ignore this version conflict");
		// browser#103: NOT XRT_ERROR_IPC_FAILURE. A client that reconnects
		// mid-session (a service restart, an in-place upgrade) has to tell
		// "retry in a moment" apart from "these two binaries will never agree";
		// the generic code made skew indistinguishable from a dozen transient
		// failures at xrCreateInstance. The log line above still names both tags.
		return XRT_ERROR_IPC_VERSION_SKEW;
	}

	// Error is ignored.
	return XRT_SUCCESS;
}

static xrt_result_t
ipc_client_describe_client(struct ipc_connection *ipc_c, const struct xrt_application_info *a_info)
{
#ifdef XRT_OS_WINDOWS
	DWORD pid = GetCurrentProcessId();
#else
	pid_t pid = getpid();
#endif

	struct ipc_client_description desc = {0};
	desc.info = *a_info;
	desc.pid = pid; // Extra info.

	xrt_result_t xret = ipc_call_instance_describe_client(ipc_c, &desc);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ipc_c, "Failed to set instance description!");
		return xret;
	}

	return XRT_SUCCESS;
}


/*
 *
 * 'Exported' functions.
 *
 */

xrt_result_t
ipc_client_connection_export(enum u_logging_level log_level, xrt_ipc_handle_t *out_handle)
{
	if (out_handle == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_handle = XRT_IPC_HANDLE_INVALID;

	// Only the message channel's log_level is read by the connect helpers; no
	// part of the connection struct outlives this call.
	struct ipc_connection scratch = {0};
	scratch.imc.ipc_handle = XRT_IPC_HANDLE_INVALID;
	scratch.imc.log_level = log_level;

#if defined(XRT_OS_WINDOWS)
	// A fresh pipe INSTANCE — the caller's own connection is untouched. No
	// auto-launch fallback on purpose: an export only makes sense while a
	// service is already up (the exporter is holding a live connection to it),
	// and a broker must never spawn.
	HANDLE pipe_inst = ipc_open_pipe_busy_retry(&scratch, IPC_WINDOWS_PIPE_NAME);
	if (pipe_inst == INVALID_HANDLE_VALUE) {
		DWORD err = GetLastError();
		U_LOG_E("ipc_client_connection_export: CreateFileA(%s) failed: %lu %s (browser#103).",
		        IPC_WINDOWS_PIPE_NAME, err, ipc_winerror(err));
		return XRT_ERROR_IPC_FAILURE;
	}
	if (!ipc_client_set_pipe_message_mode(&scratch, pipe_inst)) {
		CloseHandle(pipe_inst);
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_handle = pipe_inst;
	U_LOG_W(
	    "ipc_client_connection_export: exported an un-handshaken service pipe endpoint %p — the ADOPTING "
	    "process must run setup_shm / git-tag / describe_client (browser#103).",
	    pipe_inst);
	return XRT_SUCCESS;

#elif defined(XRT_OS_ANDROID)
	// The Android connect is Java and needs the JavaVM + Context that only
	// xrCreateInstance was given (#1056). An Android embedder brokers by doing
	// that Java connect in the process that owns the Java world and shipping the
	// fd — which is the mechanism DXR_IPC_FD / adopt_fd already ships, so there
	// is nothing here to wrap. Left unimplemented rather than faked.
	(void)log_level;
	U_LOG_E(
	    "ipc_client_connection_export: not implemented on Android — broker with the Java connect + "
	    "DXR_IPC_FD instead (#1056).");
	return XRT_ERROR_NOT_IMPLEMENTED;

#else
	int fd = ipc_client_posix_dial(&scratch);
	if (fd < 0) {
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_handle = fd;
	U_LOG_W(
	    "ipc_client_connection_export: exported an un-handshaken service socket fd %d — the ADOPTING "
	    "process must run setup_shm / git-tag / describe_client (browser#103).",
	    fd);
	return XRT_SUCCESS;
#endif
}

xrt_result_t
ipc_client_connection_init(struct ipc_connection *ipc_c,
                           enum u_logging_level log_level,
                           const struct xrt_instance_info *i_info)
{
	xrt_result_t xret;

	U_ZERO(ipc_c);
	ipc_c->imc.ipc_handle = XRT_IPC_HANDLE_INVALID;
	ipc_c->imc.log_level = log_level;
	ipc_c->ism_handle = XRT_SHMEM_HANDLE_INVALID;

	// Must be done first.
	int ret = os_mutex_init(&ipc_c->mutex);
	if (ret != 0) {
		U_LOG_E("Failed to init mutex!");
		return XRT_ERROR_IPC_FAILURE;
	}

	// Connect the service.
#ifdef XRT_OS_ANDROID
	struct _JavaVM *vm = i_info->platform_info.vm;
	void *context = i_info->platform_info.context;

	if (!ipc_client_socket_connect(ipc_c, vm, context)) {
#else
	if (!ipc_client_socket_connect(ipc_c)) {
#endif
		IPC_ERROR(ipc_c,
		          "Failed to connect to DisplayXR service process\n\n"
		          "###\n"
		          "#\n"
		          "# Please make sure that the service process is running\n"
		          "#\n"
		          "# It is called \"displayxr-service\"\n"
		          "# In build trees, it is located "
		          "\"build-dir/src/xrt/targets/service/displayxr-service\"\n"
		          "#\n"
		          "###");
		os_mutex_destroy(&ipc_c->mutex);
		return XRT_ERROR_IPC_FAILURE;
	}

	// browser#103 RC-1: MUST precede ipc_client_setup_shm — that is the call
	// that transfers the first handle, and a declaration afterwards is too late
	// (the server refuses it). Only an adopted connection sends anything here,
	// so the wire for every other client is unchanged.
	if (ipc_c->adopted) {
		ipc_client_declare_peer(ipc_c);
	}

	// Do this first so we can use it to check git tags.
	xret = ipc_client_setup_shm(ipc_c);
	if (xret != XRT_SUCCESS) {
		goto err_fini; // Already logged.
	}

	// Requires shm.
	xret = ipc_client_check_git_tag(ipc_c);
	if (xret != XRT_SUCCESS) {
		goto err_fini; // Already logged.
	}

	// Do this last.
	xret = ipc_client_describe_client(ipc_c, &i_info->app_info);
	if (xret != XRT_SUCCESS) {
		goto err_fini; // Already logged.
	}

	return XRT_SUCCESS;

err_fini:
	ipc_client_connection_fini(ipc_c);

	return xret;
}

void
ipc_client_connection_fini(struct ipc_connection *ipc_c)
{
	if (ipc_c->ism_handle != XRT_SHMEM_HANDLE_INVALID) {
		/// @todo how to tear down the shared memory?
	}
	ipc_message_channel_close(&ipc_c->imc);
	os_mutex_destroy(&ipc_c->mutex);

#ifdef XRT_OS_ANDROID
	ipc_client_android_destroy(&(ipc_c->ica));
#endif
}
