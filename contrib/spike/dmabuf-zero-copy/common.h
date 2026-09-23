// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Shared wire protocol, test pattern and fd helpers for the
 *        cross-process dma-buf zero-copy spike (epic #1699).
 *
 * NOT part of the runtime build. See README in RESULTS.md.
 */
#pragma once

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SPIKE_MAGIC 0x44585242u /* 'DXRB' */
#define SPIKE_MAX_PLANES 4

enum spike_msg_type
{
	SPIKE_MSG_FRAME = 1,
	SPIKE_MSG_QUIT = 2,
};

/* producer -> consumer. Carries num_planes plane fds, then (if has_fence) one acquire sync fd. */
struct spike_frame_msg
{
	uint32_t magic;
	uint32_t type;
	uint32_t frame;
	uint32_t bo_id; /* stable per gbm_bo; lets the consumer (optionally) cache imports */
	uint32_t width;
	uint32_t height;
	uint32_t fourcc;
	uint32_t num_planes;
	uint64_t modifier;
	uint32_t stride[SPIKE_MAX_PLANES];
	uint32_t offset[SPIKE_MAX_PLANES];
	int32_t has_fence;
	int32_t pad;
};

/* consumer -> producer. Carries one release sync fd if has_fence. */
struct spike_reply_msg
{
	uint32_t magic;
	uint32_t frame;
	int32_t has_fence;
	int32_t status; /* 0 ok, <0 import/sample failure (consumer continues) */
};

/*
 * Deterministic test pattern, byte values in R,G,B,A order (logical channels,
 * independent of the DRM memory order). Must match the GLSL in producer.c exactly.
 */
static inline void
spike_pattern(int x, int y, int f, int w, int h, uint8_t out[4])
{
	if (x < 4 || y < 4 || x >= w - 4 || y >= h - 4) {
		out[0] = 255, out[1] = 0, out[2] = 255, out[3] = 255;
		return;
	}
	int fm = f % 251;
	if ((((x >> 5) + (y >> 5)) & 1) == 1) {
		out[0] = (uint8_t)fm, out[1] = (uint8_t)(255 - fm), out[2] = (uint8_t)((x ^ y) & 255), out[3] = 255;
	} else {
		out[0] = (uint8_t)(x & 255), out[1] = (uint8_t)(y & 255), out[2] = (uint8_t)((f * 7) & 255),
		out[3] = 192;
	}
}

static inline uint64_t
spike_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline int
spike_count_fds(void)
{
	DIR *d = opendir("/proc/self/fd");
	if (!d)
		return -1;
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL)
		if (e->d_name[0] != '.')
			n++;
	closedir(d);
	return n - 1; /* minus the DIR's own fd */
}

static inline const char *
spike_default_socket(void)
{
	static char buf[256];
	const char *rt = getenv("XDG_RUNTIME_DIR");
	snprintf(buf, sizeof(buf), "%s/dxr-dmabuf-spike.sock", rt ? rt : "/tmp");
	return buf;
}

/* Send a message with up to 8 fds. Returns 0 on success. */
static inline int
spike_send(int sock, const void *data, size_t len, const int *fds, int nfds)
{
	struct iovec iov = {.iov_base = (void *)data, .iov_len = len};
	char cbuf[CMSG_SPACE(sizeof(int) * 8)];
	memset(cbuf, 0, sizeof(cbuf));
	struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};
	if (nfds > 0) {
		mh.msg_control = cbuf;
		mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
		struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
		memcpy(CMSG_DATA(c), fds, sizeof(int) * nfds);
	}
	ssize_t r = sendmsg(sock, &mh, MSG_NOSIGNAL);
	return r == (ssize_t)len ? 0 : -1;
}

/* Receive a message; fills fds (up to max) and *nfds. Returns bytes or -1. */
static inline ssize_t
spike_recv(int sock, void *data, size_t len, int *fds, int max, int *nfds)
{
	struct iovec iov = {.iov_base = data, .iov_len = len};
	char cbuf[CMSG_SPACE(sizeof(int) * 8)];
	struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf)};
	ssize_t r = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
	*nfds = 0;
	if (r <= 0)
		return r;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
			int n = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
			int *in = (int *)CMSG_DATA(c);
			for (int i = 0; i < n; i++) {
				if (*nfds < max)
					fds[(*nfds)++] = in[i];
				else
					close(in[i]);
			}
		}
	}
	if (mh.msg_flags & MSG_CTRUNC)
		fprintf(stderr, "spike_recv: WARNING control data truncated (fds lost)\n");
	return r;
}

/* Histogram of fd link targets (e.g. "anon_inode:sync_file x3"), for leak triage. */
static inline void
spike_dump_fds(const char *who)
{
	char names[64][96];
	int counts[64], n = 0;
	DIR *d = opendir("/proc/self/fd");
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;
		char path[300], tgt[96] = {0};
		snprintf(path, sizeof(path), "/proc/self/fd/%s", e->d_name);
		ssize_t l = readlink(path, tgt, sizeof(tgt) - 1);
		if (l < 0)
			continue;
		if (!strncmp(tgt, "socket:", 7) || !strncmp(tgt, "pipe:", 5))
			tgt[strcspn(tgt, "[")] = 0;
		int i;
		for (i = 0; i < n; i++)
			if (!strcmp(names[i], tgt))
				break;
		if (i == n && n < 64)
			snprintf(names[n], 96, "%s", tgt), counts[n++] = 0;
		if (i < 64)
			counts[i]++;
	}
	closedir(d);
	printf("%s: fd table:", who);
	for (int i = 0; i < n; i++)
		printf(" [%s x%d]", names[i], counts[i]);
	printf("\n");
}
