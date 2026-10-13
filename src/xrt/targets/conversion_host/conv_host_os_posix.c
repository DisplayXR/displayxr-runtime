// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host, POSIX process duties: elevation check,
 *         core dumps, parent-death watch.
 * @ingroup conv_host
 */

#include "conv_host_os.h"

#include "xrt/xrt_config_os.h"
#include "util/u_logging.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(XRT_OS_LINUX)
#include <sys/prctl.h>
#endif

#if defined(XRT_OS_MACOS)
#include <pthread.h>
#include <sys/event.h>
#endif

bool
conv_host_os_is_elevated(char *out_detail, uint32_t detail_size)
{
	const uid_t ruid = getuid();
	const uid_t euid = geteuid();
	if (out_detail != NULL && detail_size > 0) {
		(void)snprintf(out_detail, detail_size, "uid=%lu euid=%lu", (unsigned long)ruid, (unsigned long)euid);
	}
	return euid == 0 || euid != ruid;
}

void
conv_host_os_install_crash_handler(const char *provider_id)
{
	(void)provider_id;
	// Let the system core pattern (systemd-coredump, /cores) capture a crash.
	struct rlimit rl;
	if (getrlimit(RLIMIT_CORE, &rl) == 0 && rl.rlim_cur != rl.rlim_max) {
		rl.rlim_cur = rl.rlim_max;
		(void)setrlimit(RLIMIT_CORE, &rl);
	}
}

bool
conv_host_os_start_session_end_watch(void (*on_close)(void *ctx), void *ctx, uint32_t backstop_ms)
{
	(void)on_close;
	(void)ctx;
	(void)backstop_ms;
	return true; // no Restart Manager; the supervisor closes us over the channel
}

void *
conv_host_os_session_end_hwnd(void)
{
	return NULL;
}

#if defined(XRT_OS_MACOS)
static void *
parent_watch_thread(void *p)
{
	const pid_t ppid = (pid_t)(intptr_t)p;
	const int kq = kqueue();
	if (kq < 0) {
		return NULL;
	}
	struct kevent ev;
	EV_SET(&ev, ppid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
	if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
		// ESRCH: already gone.
		_exit(0);
	}
	struct kevent out;
	while (kevent(kq, NULL, 0, &out, 1, NULL) < 0) {
	}
	_exit(0); // the service is gone: nothing left to serve
	return NULL;
}
#endif

bool
conv_host_os_watch_parent(uint64_t parent_pid)
{
#if defined(XRT_OS_LINUX)
	(void)prctl(PR_SET_PDEATHSIG, SIGKILL);
	// The parent may have died between fork and prctl.
	return (uint64_t)getppid() == parent_pid;
#elif defined(XRT_OS_MACOS)
	if ((uint64_t)getppid() != parent_pid) {
		return false;
	}
	pthread_t t;
	if (pthread_create(&t, NULL, parent_watch_thread, (void *)(intptr_t)parent_pid) == 0) {
		pthread_detach(t);
	}
	return true;
#else
	(void)parent_pid;
	return true;
#endif
}

uint64_t
conv_host_os_pid(void)
{
	return (uint64_t)getpid();
}
