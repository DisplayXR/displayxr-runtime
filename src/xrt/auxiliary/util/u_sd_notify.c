// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Minimal systemd notify protocol (sd_notify) without libsystemd (#2515).
 * @ingroup aux_util
 */

#include "xrt/xrt_config_os.h"

#include "util/u_sd_notify.h"
#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_LINUX_DESKTOP
#include <errno.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

size_t
u_sd_notify_parse_socket(const char *value, char *out_path, size_t out_size)
{
	if (value == NULL || out_path == NULL || out_size == 0) {
		return 0;
	}
	const size_t len = strlen(value);
	if (len < 2) {
		return 0;
	}
	if (value[0] == '@') {
		// Abstract namespace: NUL + name, length-delimited (no terminator).
		if (len > out_size) {
			return 0;
		}
		out_path[0] = '\0';
		memcpy(out_path + 1, value + 1, len - 1);
		return len;
	}
	if (value[0] == '/') {
		if (len + 1 > out_size) {
			return 0;
		}
		memcpy(out_path, value, len + 1);
		return len + 1;
	}
	return 0;
}

static bool
parse_u64(const char *s, uint64_t *out)
{
	if (s == NULL || *s == '\0') {
		return false;
	}
	uint64_t v = 0;
	for (const char *p = s; *p != '\0'; p++) {
		if (*p < '0' || *p > '9') {
			return false;
		}
		const uint64_t d = (uint64_t)(*p - '0');
		if (v > (UINT64_MAX - d) / 10) {
			return false;
		}
		v = v * 10 + d;
	}
	*out = v;
	return true;
}

uint64_t
u_sd_notify_parse_watchdog_usec(const char *watchdog_usec, const char *watchdog_pid, long self_pid)
{
	uint64_t usec = 0;
	if (!parse_u64(watchdog_usec, &usec) || usec == 0) {
		return 0;
	}
	if (watchdog_pid != NULL && *watchdog_pid != '\0') {
		uint64_t pid = 0;
		if (!parse_u64(watchdog_pid, &pid) || pid != (uint64_t)self_pid) {
			return 0;
		}
	}
	return usec;
}

#ifdef XRT_OS_LINUX_DESKTOP

static char g_addr_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static size_t g_addr_len = 0;      //!< 0: not under systemd
static uint64_t g_watchdog_ns = 0; //!< 0: no watchdog
static uint64_t g_last_ping_ns = 0;
static bool g_initialized = false;

void
u_sd_notify_init(void)
{
	if (g_initialized) {
		return;
	}
	g_initialized = true;

	const char *sock = getenv("NOTIFY_SOCKET");
	if (sock != NULL) {
		g_addr_len = u_sd_notify_parse_socket(sock, g_addr_path, sizeof(g_addr_path));
		if (g_addr_len == 0) {
			U_LOG_W("NOTIFY_SOCKET '%s' is not a usable AF_UNIX address; systemd notifications are off",
			        sock);
		}
	}
	const uint64_t usec =
	    u_sd_notify_parse_watchdog_usec(getenv("WATCHDOG_USEC"), getenv("WATCHDOG_PID"), (long)getpid());
	if (g_addr_len != 0 && usec != 0) {
		g_watchdog_ns = usec * 1000u;
		U_LOG_W("systemd watchdog armed: %llu ms (pinging every %llu ms)", (unsigned long long)(usec / 1000u),
		        (unsigned long long)(usec / 2000u));
	}

	// Not for children (the workspace controller, satellites): with
	// NotifyAccess=main their messages would be ignored anyway, but a child
	// that speaks the protocol would otherwise believe it owns the watchdog.
	unsetenv("NOTIFY_SOCKET");
	unsetenv("WATCHDOG_USEC");
	unsetenv("WATCHDOG_PID");
}

bool
u_sd_notify_send(const char *state)
{
	if (g_addr_len == 0 || state == NULL) {
		return false;
	}
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return false;
	}
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, g_addr_path, g_addr_len);
	const socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + g_addr_len);

	const size_t n = strlen(state);
	ssize_t ret;
	do {
		ret = sendto(fd, state, n, MSG_NOSIGNAL, (const struct sockaddr *)&addr, addr_len);
	} while (ret < 0 && errno == EINTR);
	close(fd);
	return ret == (ssize_t)n;
}

bool
u_sd_notify_watchdog_enabled(void)
{
	return g_watchdog_ns != 0;
}

void
u_sd_notify_watchdog_tick(uint64_t now_ns)
{
	if (g_watchdog_ns == 0) {
		return;
	}
	if (g_last_ping_ns != 0 && now_ns - g_last_ping_ns < g_watchdog_ns / 2) {
		return;
	}
	g_last_ping_ns = now_ns;
	(void)u_sd_notify_send("WATCHDOG=1");
}

#else // !XRT_OS_LINUX_DESKTOP

void
u_sd_notify_init(void)
{}

bool
u_sd_notify_send(const char *state)
{
	(void)state;
	return false;
}

bool
u_sd_notify_watchdog_enabled(void)
{
	return false;
}

void
u_sd_notify_watchdog_tick(uint64_t now_ns)
{
	(void)now_ns;
}

#endif // XRT_OS_LINUX_DESKTOP
