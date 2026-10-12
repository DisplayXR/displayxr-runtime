// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-Linux display hotplug feeder — see service_drm_hotplug_linux.h.
 * @ingroup ipc
 */

#include "service_drm_hotplug_linux.h"

#include "util/u_logging.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


//! The polling fallback's period.
#define DRM_POLL_PERIOD_MS 2000
//! Connectors the polling fallback tracks (a desktop has a handful).
#define DRM_POLL_MAX_CONNECTORS 64
//! Path of the DRM class directory (connectors are `card<N>-<connector>`).
#define DRM_CLASS_DIR "/sys/class/drm"


/*
 *
 * Pure helpers.
 *
 */

bool
service_drm_uevent_is_hotplug(const char *buf, size_t len)
{
	if (buf == NULL || len == 0) {
		return false;
	}
	// udev re-broadcasts on group 2 with a binary "libudev" header; only the
	// kernel's own text uevents (group 1) are parsed here.
	if (len >= 8 && memcmp(buf, "libudev", 8) == 0) {
		return false;
	}
	bool drm = false;
	bool change = false;
	size_t i = 0;
	while (i < len) {
		const char *tok = buf + i;
		const size_t max = len - i;
		const size_t tl = strnlen(tok, max);
		if (tl == 13 && strncmp(tok, "ACTION=change", tl) == 0) {
			change = true;
		} else if (tl == 13 && strncmp(tok, "SUBSYSTEM=drm", tl) == 0) {
			drm = true;
		}
		i += tl + 1; // the first token is the "ACTION@DEVPATH" header; it matches neither
	}
	return drm && change;
}

uint64_t
service_drm_status_hash(const char *const *names, const char *const *statuses, uint32_t count)
{
	uint64_t h = 1469598103934665603ull;
	for (uint32_t i = 0; i < count; i++) {
		const char *parts[2] = {names != NULL ? names[i] : NULL, statuses != NULL ? statuses[i] : NULL};
		for (int p = 0; p < 2; p++) {
			const char *s = parts[p] != NULL ? parts[p] : "";
			for (; *s != '\0'; s++) {
				h = (h ^ (uint8_t)*s) * 1099511628211ull;
			}
			h = (h ^ 0xffu) * 1099511628211ull; // separator: "ab"+"c" != "a"+"bc"
		}
	}
	return h;
}


/*
 *
 * Polling fallback.
 *
 */

static int
cmp_str(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

//! Hash of every connector's status now (0 connectors hash too: a stable value).
static uint64_t
sysfs_status_hash(void)
{
	char names_buf[DRM_POLL_MAX_CONNECTORS][64];
	char status_buf[DRM_POLL_MAX_CONNECTORS][32];
	const char *names[DRM_POLL_MAX_CONNECTORS];
	uint32_t n = 0;

	DIR *d = opendir(DRM_CLASS_DIR);
	if (d != NULL) {
		struct dirent *e;
		while ((e = readdir(d)) != NULL && n < DRM_POLL_MAX_CONNECTORS) {
			// Connectors only: "card1-eDP-1", not "card1" / "renderD128" / "version".
			const size_t nl = strlen(e->d_name);
			if (strncmp(e->d_name, "card", 4) != 0 || strchr(e->d_name, '-') == NULL ||
			    nl >= sizeof(names_buf[n])) {
				continue;
			}
			memcpy(names_buf[n], e->d_name, nl + 1);
			names[n] = names_buf[n];
			n++;
		}
		closedir(d);
	}
	qsort(names, n, sizeof(names[0]), cmp_str);

	const char *statuses[DRM_POLL_MAX_CONNECTORS];
	for (uint32_t i = 0; i < n; i++) {
		status_buf[i][0] = '\0';
		char path[160];
		(void)snprintf(path, sizeof(path), DRM_CLASS_DIR "/%s/status", names[i]);
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			ssize_t r = read(fd, status_buf[i], sizeof(status_buf[i]) - 1);
			status_buf[i][r > 0 ? r : 0] = '\0';
			close(fd);
		}
		statuses[i] = status_buf[i];
	}
	return service_drm_status_hash(names, statuses, n);
}


/*
 *
 * The thread.
 *
 */

static pthread_t g_thread;
static bool g_running = false;
static int g_stop_pipe[2] = {-1, -1};
static int g_nl_fd = -1;
static service_drm_hotplug_fn g_fn = NULL;

static int
open_uevent_socket(void)
{
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
	if (fd < 0) {
		return -errno;
	}
	struct sockaddr_nl addr;
	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_pid = 0;    // let the kernel pick a unique port id
	addr.nl_groups = 1; // the kernel's uevent multicast group
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		const int err = errno;
		close(fd);
		return -err;
	}
	return fd;
}

//! Drain the socket; true if any datagram was a DRM change from the kernel.
static bool
drain_uevents(int fd)
{
	bool hit = false;
	char buf[8192];
	for (;;) {
		struct sockaddr_nl from;
		struct iovec iov = {.iov_base = buf, .iov_len = sizeof(buf)};
		struct msghdr msg;
		memset(&msg, 0, sizeof(msg));
		memset(&from, 0, sizeof(from));
		msg.msg_name = &from;
		msg.msg_namelen = sizeof(from);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		const ssize_t r = recvmsg(fd, &msg, MSG_DONTWAIT);
		if (r <= 0) {
			break; // EAGAIN: drained (or an error: the next poll retries)
		}
		// Kernel uevents come from port id 0; anything else is a user process.
		if (from.nl_pid != 0 || (msg.msg_flags & MSG_TRUNC) != 0) {
			continue;
		}
		if (service_drm_uevent_is_hotplug(buf, (size_t)r)) {
			hit = true;
		}
	}
	return hit;
}

static void *
thread_func(void *ptr)
{
	(void)ptr;
	uint64_t last_hash = 0;
	const bool netlink = g_nl_fd >= 0;
	if (!netlink) {
		last_hash = sysfs_status_hash();
	}

	for (;;) {
		struct pollfd fds[2];
		fds[0].fd = g_stop_pipe[0];
		fds[0].events = POLLIN;
		fds[0].revents = 0;
		fds[1].fd = g_nl_fd;
		fds[1].events = POLLIN;
		fds[1].revents = 0;
		const int r = poll(fds, netlink ? 2 : 1, netlink ? -1 : DRM_POLL_PERIOD_MS);
		if (r < 0 && errno != EINTR) {
			U_LOG_W("display hotplug feeder: poll failed (%s); stopping.", strerror(errno));
			break;
		}
		if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
			break; // stop requested
		}
		if (netlink) {
			if ((fds[1].revents & POLLIN) != 0 && drain_uevents(g_nl_fd)) {
				g_fn("drm hotplug");
			}
			continue;
		}
		const uint64_t h = sysfs_status_hash();
		if (h != last_hash) {
			last_hash = h;
			g_fn("drm connector status");
		}
	}
	return NULL;
}

bool
service_drm_hotplug_start(service_drm_hotplug_fn fn)
{
	if (fn == NULL || g_running) {
		return false;
	}
	if (pipe2(g_stop_pipe, O_CLOEXEC) != 0) {
		U_LOG_W("display hotplug feeder: pipe failed (%s); display changes re-probe only on client events.",
		        strerror(errno));
		return false;
	}
	g_fn = fn;
	// DXR_DRM_HOTPLUG_POLL=1 forces the polling fallback (tests, odd sandboxes).
	const char *force_poll = getenv("DXR_DRM_HOTPLUG_POLL");
	const int fd = (force_poll != NULL && force_poll[0] == '1') ? -EPERM : open_uevent_socket();
	g_nl_fd = fd >= 0 ? fd : -1;

	if (pthread_create(&g_thread, NULL, thread_func, NULL) != 0) {
		U_LOG_W(
		    "display hotplug feeder: thread failed to start; display changes re-probe only on client events.");
		if (g_nl_fd >= 0) {
			close(g_nl_fd);
			g_nl_fd = -1;
		}
		close(g_stop_pipe[0]);
		close(g_stop_pipe[1]);
		g_stop_pipe[0] = g_stop_pipe[1] = -1;
		return false;
	}
	(void)pthread_setname_np(g_thread, "dxr-drm-hotplug");
	g_running = true;

	if (g_nl_fd >= 0) {
		U_LOG_W(
		    "display hotplug feeder: kernel uevents (NETLINK_KOBJECT_UEVENT, SUBSYSTEM=drm ACTION=change) "
		    "-> display re-probe (ADR-051 D6)");
	} else {
		U_LOG_W("display hotplug feeder: uevent socket unavailable (%s); polling " DRM_CLASS_DIR
		        "/card*-*/status every %d ms -> display re-probe (ADR-051 D6)",
		        fd == -EPERM && force_poll != NULL && force_poll[0] == '1' ? "DXR_DRM_HOTPLUG_POLL=1"
		                                                                   : strerror(-fd),
		        DRM_POLL_PERIOD_MS);
	}
	return true;
}

void
service_drm_hotplug_stop(void)
{
	if (!g_running) {
		return;
	}
	g_running = false;
	const char b = 1;
	(void)!write(g_stop_pipe[1], &b, 1);
	pthread_join(g_thread, NULL);
	if (g_nl_fd >= 0) {
		close(g_nl_fd);
		g_nl_fd = -1;
	}
	close(g_stop_pipe[0]);
	close(g_stop_pipe[1]);
	g_stop_pipe[0] = g_stop_pipe[1] = -1;
	g_fn = NULL;
}
