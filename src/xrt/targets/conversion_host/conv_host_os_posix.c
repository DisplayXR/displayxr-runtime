// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host, POSIX process duties: core dumps and the
 *         SIGTERM close-for-update request. (The parent-death watch and the
 *         elevation check are os/os_process.h's.)
 * @ingroup conv_host
 */

#include "conv_host_os.h"
#include "conv_host_exit.h"

#include "util/u_logging.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

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

/*
 *
 * SIGTERM = close for update (the POSIX stand-in for Restart Manager's close
 * request, roadmap §3.6): a package manager or an installer stops the host,
 * never the service. The service itself never sends SIGTERM (it closes the
 * host over the channel, or SIGKILLs it). The handler only writes one byte to
 * a self-pipe (async-signal-safe); a watcher thread does the rest.
 *
 */

static struct
{
	void (*on_close)(void *ctx);
	void *ctx;
	uint32_t backstop_ms;
	int pipe_fd[2];
} g_se = {NULL, NULL, 0, {-1, -1}};

static void
sigterm_handler(int sig)
{
	(void)sig;
	const int saved = errno;
	const char b = 1;
	if (g_se.pipe_fd[1] >= 0) {
		(void)!write(g_se.pipe_fd[1], &b, 1);
	}
	errno = saved;
}

static void *
sigterm_watch(void *arg)
{
	(void)arg;
	char b = 0;
	ssize_t n;
	do {
		n = read(g_se.pipe_fd[0], &b, 1);
	} while (n < 0 && errno == EINTR);
	if (n != 1) {
		return NULL;
	}
	U_LOG_W("conversion host: SIGTERM: closing for update (exit 20)");
	if (g_se.on_close != NULL) {
		g_se.on_close(g_se.ctx);
	}
	// Backstop: still alive after the grace period -> leave with 20 anyway.
	struct timespec ts = {(time_t)(g_se.backstop_ms / 1000u), (long)(g_se.backstop_ms % 1000u) * 1000000L};
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
	}
	U_LOG_W("conversion host: still alive %u ms after SIGTERM; exiting 20 now", g_se.backstop_ms);
	_exit(CONV_HOST_EXIT_UPDATE);
	return NULL;
}

bool
conv_host_os_start_session_end_watch(void (*on_close)(void *ctx), void *ctx, uint32_t backstop_ms)
{
	g_se.on_close = on_close;
	g_se.ctx = ctx;
	g_se.backstop_ms = backstop_ms;
	if (pipe(g_se.pipe_fd) != 0) {
		return false;
	}
	pthread_t t;
	if (pthread_create(&t, NULL, sigterm_watch, NULL) != 0) {
		return false;
	}
	(void)pthread_detach(t);
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigterm_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	return sigaction(SIGTERM, &sa, NULL) == 0;
}

void *
conv_host_os_session_end_hwnd(void)
{
	return NULL;
}
