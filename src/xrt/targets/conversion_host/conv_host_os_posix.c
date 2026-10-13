// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host, POSIX process duties: core dumps. (The
 *         parent-death watch and the elevation check are os/os_process.h's.)
 * @ingroup conv_host
 */

#include "conv_host_os.h"

#include <stddef.h>
#include <sys/resource.h>

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
