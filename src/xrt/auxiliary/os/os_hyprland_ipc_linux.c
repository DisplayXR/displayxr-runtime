// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Hyprland IPC sockets (desktop Linux). See os_hyprland.h.
 * @ingroup aux_os
 */

#include "os_hyprland.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define HYPR_MAX_REPLY (8u * 1024u * 1024u)

static int
connect_unix(const char *dir, const char *leaf)
{
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	const int n = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s", dir, leaf);
	if (n <= 0 || (size_t)n >= sizeof(addr.sun_path)) {
		return -1;
	}
	const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool
dir_is_live(const char *dir)
{
	const int fd = connect_unix(dir, ".socket.sock");
	if (fd < 0) {
		return false;
	}
	close(fd); // an empty request is ignored by Hyprland
	return true;
}

bool
os_hyprland_socket_dir(char *out, size_t out_size)
{
	if (out == NULL || out_size == 0) {
		return false;
	}
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");

	if (sig != NULL && sig[0] != '\0' && strchr(sig, '/') == NULL) {
		if (runtime != NULL && runtime[0] != '\0') {
			snprintf(out, out_size, "%s/hypr/%s", runtime, sig);
			if (dir_is_live(out)) {
				return true;
			}
		}
		snprintf(out, out_size, "/tmp/hypr/%s", sig); // Hyprland < 0.40
		if (dir_is_live(out)) {
			return true;
		}
	}

	// No (or a stale) signature: take the one live instance, if exactly one.
	if (runtime == NULL || runtime[0] == '\0') {
		return false;
	}
	char base[512];
	snprintf(base, sizeof(base), "%s/hypr", runtime);
	DIR *d = opendir(base);
	if (d == NULL) {
		return false;
	}
	int live = 0;
	char found[512] = "";
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char cand[512];
		const int n = snprintf(cand, sizeof(cand), "%s/%s", base, e->d_name);
		if (n <= 0 || (size_t)n >= sizeof(cand)) {
			continue;
		}
		if (dir_is_live(cand)) {
			live++;
			snprintf(found, sizeof(found), "%s", cand);
		}
	}
	closedir(d);
	if (live != 1) {
		return false; // none, or ambiguous (two sessions of one user)
	}
	snprintf(out, out_size, "%s", found);
	return true;
}

char *
os_hyprland_request(const char *dir, const char *request, int timeout_ms)
{
	if (dir == NULL || request == NULL) {
		return NULL;
	}
	const int fd = connect_unix(dir, ".socket.sock");
	if (fd < 0) {
		return NULL;
	}
	const size_t req_len = strlen(request);
	size_t sent = 0;
	while (sent < req_len) {
		const ssize_t w = send(fd, request + sent, req_len - sent, MSG_NOSIGNAL);
		if (w < 0 && errno == EINTR) {
			continue;
		}
		if (w <= 0) {
			close(fd);
			return NULL;
		}
		sent += (size_t)w;
	}

	size_t cap = 16384, len = 0;
	char *buf = malloc(cap);
	if (buf == NULL) {
		close(fd);
		return NULL;
	}
	for (;;) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		const int pr = poll(&pfd, 1, timeout_ms);
		if (pr < 0 && errno == EINTR) {
			continue;
		}
		if (pr <= 0) {
			free(buf);
			close(fd);
			return NULL; // timeout: never leave Hyprland's request half-read for long
		}
		if (len + 4097 > cap) {
			if (cap >= HYPR_MAX_REPLY) {
				free(buf);
				close(fd);
				return NULL;
			}
			cap *= 2;
			char *nb = realloc(buf, cap);
			if (nb == NULL) {
				free(buf);
				close(fd);
				return NULL;
			}
			buf = nb;
		}
		const ssize_t r = recv(fd, buf + len, cap - len - 1, 0);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r < 0) {
			free(buf);
			close(fd);
			return NULL;
		}
		if (r == 0) {
			break; // the compositor closes after the reply
		}
		len += (size_t)r;
	}
	close(fd);
	buf[len] = '\0';
	return buf;
}

int
os_hyprland_event_connect(const char *dir)
{
	if (dir == NULL) {
		return -1;
	}
	const int fd = connect_unix(dir, ".socket2.sock");
	if (fd < 0) {
		return -1;
	}
	const int fl = fcntl(fd, F_GETFL, 0);
	if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}
