// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host control channel + lane primitives.
 * @ingroup conv_host
 */

#include "conv_host_chan.h"

#include "xrt/xrt_config_os.h"
#include "os/os_threading.h"
#include "util/u_logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

static void
set_reason(char *out, uint32_t size, const char *msg, long code)
{
	if (out != NULL && size > 0) {
		(void)snprintf(out, size, "%s (%ld)", msg, code);
	}
}

static bool
token_is_number(const char *s, uint64_t *out)
{
	if (s == NULL || *s < '0' || *s > '9') {
		return false;
	}
	uint64_t v = 0;
	for (const char *c = s; *c != '\0'; c++) {
		if (*c < '0' || *c > '9') {
			return false;
		}
		v = v * 10u + (uint64_t)(*c - '0');
	}
	*out = v;
	return true;
}

#ifdef XRT_OS_WINDOWS

/*
 *
 * Windows: a named-pipe client handle (or an inherited pipe handle), with
 * overlapped I/O so a blocked read never serialises a send from another
 * thread.
 *
 */

struct conv_host_chan
{
	HANDLE pipe;
	HANDLE read_ev;
	HANDLE write_ev;
	struct os_mutex send_mtx;
	volatile LONG dead;
};

struct conv_host_chan *
conv_host_chan_open(const char *token, char *out_reason, uint32_t reason_size)
{
	HANDLE h = INVALID_HANDLE_VALUE;
	uint64_t v = 0;
	if (token_is_number(token, &v)) {
		h = (HANDLE)(uintptr_t)v;
		if (GetFileType(h) != FILE_TYPE_PIPE) {
			set_reason(out_reason, reason_size, "inherited channel token is not a pipe", (long)GetLastError());
			return NULL;
		}
	} else {
		// os_channel_create_for_child hands out either the bare name or the
		// full `\\.\pipe\...` path: accept both.
		const bool full = strncmp(token, "\\\\.\\pipe\\", 9) == 0;
		wchar_t name[300];
		if (_snwprintf_s(name, ARRAYSIZE(name), _TRUNCATE, full ? L"%hs" : L"\\\\.\\pipe\\%hs", token) < 0) {
			set_reason(out_reason, reason_size, "channel name too long", 0);
			return NULL;
		}
		for (int attempt = 0; attempt < 50; attempt++) {
			h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
			                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
			if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) {
				break;
			}
			(void)WaitNamedPipeW(name, 100);
		}
		if (h == INVALID_HANDLE_VALUE) {
			set_reason(out_reason, reason_size, "connecting to the control pipe failed", (long)GetLastError());
			return NULL;
		}
		DWORD mode = PIPE_READMODE_BYTE;
		(void)SetNamedPipeHandleState(h, &mode, NULL, NULL);
	}
	struct conv_host_chan *c = calloc(1, sizeof(*c));
	if (c == NULL) {
		CloseHandle(h);
		return NULL;
	}
	c->pipe = h;
	c->read_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
	c->write_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
	(void)os_mutex_init(&c->send_mtx);
	return c;
}

static bool
io_all(struct conv_host_chan *c, bool write, void *buf, uint32_t bytes)
{
	uint8_t *p = (uint8_t *)buf;
	while (bytes > 0) {
		if (c->dead) {
			return false;
		}
		OVERLAPPED ov = {0};
		ov.hEvent = write ? c->write_ev : c->read_ev;
		ResetEvent(ov.hEvent);
		DWORD done = 0;
		BOOL ok = write ? WriteFile(c->pipe, p, bytes, &done, &ov) : ReadFile(c->pipe, p, bytes, &done, &ov);
		if (!ok) {
			const DWORD err = GetLastError();
			if (err != ERROR_IO_PENDING) {
				return false;
			}
			if (!GetOverlappedResult(c->pipe, &ov, &done, TRUE)) {
				return false;
			}
		}
		if (done == 0) {
			return false;
		}
		p += done;
		bytes -= done;
	}
	return true;
}

bool
conv_host_chan_recv(struct conv_host_chan *c, struct conv_host_wire_frame *f, void *payload)
{
	if (c == NULL || !io_all(c, false, f, (uint32_t)sizeof(*f)) || !conv_host_wire_frame_valid(f)) {
		return false;
	}
	if (f->payload_bytes > 0 && !io_all(c, false, payload, f->payload_bytes)) {
		return false;
	}
	return true;
}

bool
conv_host_chan_send(struct conv_host_chan *c,
                    uint32_t type,
                    uint32_t request_id,
                    const void *payload,
                    uint32_t payload_bytes,
                    const uint64_t *handles,
                    uint32_t handle_count)
{
	if (c == NULL || payload_bytes > CONV_HOST_WIRE_MAX_PAYLOAD || handle_count > CONV_HOST_WIRE_MAX_HANDLES) {
		return false;
	}
	uint8_t buf[sizeof(struct conv_host_wire_frame) + CONV_HOST_WIRE_MAX_PAYLOAD];
	struct conv_host_wire_frame f = {0};
	f.magic = CONV_HOST_WIRE_MAGIC;
	f.type = type;
	f.payload_bytes = payload_bytes;
	f.request_id = request_id;
	f.handle_count = handle_count;
	for (uint32_t i = 0; i < handle_count; i++) {
		f.handles[i] = handles[i];
	}
	memcpy(buf, &f, sizeof(f));
	if (payload_bytes > 0) {
		memcpy(buf + sizeof(f), payload, payload_bytes);
	}
	os_mutex_lock(&c->send_mtx);
	const bool ok = io_all(c, true, buf, (uint32_t)sizeof(f) + payload_bytes);
	os_mutex_unlock(&c->send_mtx);
	return ok;
}

void
conv_host_chan_shutdown(struct conv_host_chan *c)
{
	if (c == NULL) {
		return;
	}
	InterlockedExchange(&c->dead, 1);
	(void)CancelIoEx(c->pipe, NULL);
}

void
conv_host_chan_destroy(struct conv_host_chan **c_ptr)
{
	if (c_ptr == NULL || *c_ptr == NULL) {
		return;
	}
	struct conv_host_chan *c = *c_ptr;
	CloseHandle(c->pipe);
	CloseHandle(c->read_ev);
	CloseHandle(c->write_ev);
	os_mutex_destroy(&c->send_mtx);
	free(c);
	*c_ptr = NULL;
}

void
conv_host_handle_close(uint64_t h)
{
	if (h != 0 && (HANDLE)(uintptr_t)h != INVALID_HANDLE_VALUE) {
		CloseHandle((HANDLE)(uintptr_t)h);
	}
}

bool
conv_host_wake_wait(uint64_t h, uint32_t timeout_ms)
{
	return WaitForSingleObject((HANDLE)(uintptr_t)h, timeout_ms) == WAIT_OBJECT_0;
}

void
conv_host_wake_signal(uint64_t h)
{
	(void)SetEvent((HANDLE)(uintptr_t)h);
}

void *
conv_host_shm_map(uint64_t h, uint64_t bytes)
{
	return MapViewOfFile((HANDLE)(uintptr_t)h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, (SIZE_T)bytes);
}

void
conv_host_shm_unmap(void *p, uint64_t bytes)
{
	(void)bytes;
	if (p != NULL) {
		UnmapViewOfFile(p);
	}
}

uint64_t
conv_host_shm_create(uint64_t bytes, void **out_map)
{
	HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)(bytes >> 32),
	                              (DWORD)(bytes & 0xffffffffu), NULL);
	if (h == NULL) {
		return 0;
	}
	void *m = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, (SIZE_T)bytes);
	if (m == NULL) {
		CloseHandle(h);
		return 0;
	}
	*out_map = m;
	return (uint64_t)(uintptr_t)h;
}

#else // POSIX

/*
 *
 * POSIX: an AF_UNIX stream socket, descriptors as SCM_RIGHTS.
 *
 */

struct conv_host_chan
{
	int fd;
	struct os_mutex send_mtx;
	volatile int dead;
};

struct conv_host_chan *
conv_host_chan_open(const char *token, char *out_reason, uint32_t reason_size)
{
	int fd = -1;
	uint64_t v = 0;
	if (token_is_number(token, &v)) {
		fd = (int)v;
		int type = 0;
		socklen_t tl = sizeof(type);
		if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) != 0 || type != SOCK_STREAM) {
			set_reason(out_reason, reason_size, "inherited channel token is not a stream socket", (long)errno);
			return NULL;
		}
	} else {
		struct sockaddr_un addr;
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		socklen_t len;
		const size_t n = strlen(token);
		if (token[0] == '/') {
			if (n >= sizeof(addr.sun_path)) {
				set_reason(out_reason, reason_size, "socket path too long", 0);
				return NULL;
			}
			memcpy(addr.sun_path, token, n + 1);
			len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
		} else {
#ifdef XRT_OS_LINUX
			if (n + 1 >= sizeof(addr.sun_path)) {
				set_reason(out_reason, reason_size, "socket name too long", 0);
				return NULL;
			}
			addr.sun_path[0] = '\0'; // abstract namespace
			memcpy(addr.sun_path + 1, token, n);
			len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
#else
			set_reason(out_reason, reason_size, "a channel name must be an absolute socket path", 0);
			return NULL;
#endif
		}
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0 || connect(fd, (struct sockaddr *)&addr, len) != 0) {
			set_reason(out_reason, reason_size, "connecting to the control socket failed", (long)errno);
			if (fd >= 0) {
				close(fd);
			}
			return NULL;
		}
	}
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
	int one = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	struct conv_host_chan *c = calloc(1, sizeof(*c));
	if (c == NULL) {
		close(fd);
		return NULL;
	}
	c->fd = fd;
	(void)os_mutex_init(&c->send_mtx);
	return c;
}

//! Read exactly @p bytes; collect any SCM_RIGHTS descriptors into fds[].
static bool
recv_all(struct conv_host_chan *c, void *buf, uint32_t bytes, int *fds, uint32_t *fd_count, uint32_t fd_max)
{
	uint8_t *p = (uint8_t *)buf;
	while (bytes > 0) {
		if (c->dead) {
			return false;
		}
		struct iovec iov = {p, bytes};
		union {
			struct cmsghdr h;
			uint8_t b[CMSG_SPACE(sizeof(int) * CONV_HOST_WIRE_MAX_HANDLES)];
		} cbuf;
		struct msghdr m;
		memset(&m, 0, sizeof(m));
		m.msg_iov = &iov;
		m.msg_iovlen = 1;
		m.msg_control = cbuf.b;
		m.msg_controllen = sizeof(cbuf.b);
		const ssize_t r = recvmsg(c->fd, &m, 0);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r <= 0) {
			return false;
		}
		for (struct cmsghdr *cm = CMSG_FIRSTHDR(&m); cm != NULL; cm = CMSG_NXTHDR(&m, cm)) {
			if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) {
				continue;
			}
			const size_t n = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			const int *in = (const int *)CMSG_DATA(cm);
			for (size_t i = 0; i < n; i++) {
				if (*fd_count < fd_max) {
					fds[(*fd_count)++] = in[i];
				} else {
					close(in[i]); // surplus: never leak a peer's descriptor
				}
			}
		}
		p += r;
		bytes -= (uint32_t)r;
	}
	return true;
}

bool
conv_host_chan_recv(struct conv_host_chan *c, struct conv_host_wire_frame *f, void *payload)
{
	if (c == NULL) {
		return false;
	}
	int fds[CONV_HOST_WIRE_MAX_HANDLES];
	uint32_t nfd = 0;
	bool ok = recv_all(c, f, (uint32_t)sizeof(*f), fds, &nfd, CONV_HOST_WIRE_MAX_HANDLES) &&
	          conv_host_wire_frame_valid(f);
	if (ok && f->payload_bytes > 0) {
		ok = recv_all(c, payload, f->payload_bytes, fds, &nfd, CONV_HOST_WIRE_MAX_HANDLES);
	}
	if (!ok || nfd != f->handle_count) {
		for (uint32_t i = 0; i < nfd; i++) {
			close(fds[i]);
		}
		return false;
	}
	for (uint32_t i = 0; i < CONV_HOST_WIRE_MAX_HANDLES; i++) {
		f->handles[i] = i < nfd ? (uint64_t)fds[i] : 0;
	}
	return true;
}

bool
conv_host_chan_send(struct conv_host_chan *c,
                    uint32_t type,
                    uint32_t request_id,
                    const void *payload,
                    uint32_t payload_bytes,
                    const uint64_t *handles,
                    uint32_t handle_count)
{
	if (c == NULL || payload_bytes > CONV_HOST_WIRE_MAX_PAYLOAD || handle_count > CONV_HOST_WIRE_MAX_HANDLES) {
		return false;
	}
	struct conv_host_wire_frame f;
	memset(&f, 0, sizeof(f));
	f.magic = CONV_HOST_WIRE_MAGIC;
	f.type = type;
	f.payload_bytes = payload_bytes;
	f.request_id = request_id;
	f.handle_count = handle_count;

	struct iovec iov[2] = {{&f, sizeof(f)}, {(void *)payload, payload_bytes}};
	union {
		struct cmsghdr h;
		uint8_t b[CMSG_SPACE(sizeof(int) * CONV_HOST_WIRE_MAX_HANDLES)];
	} cbuf;
	struct msghdr m;
	memset(&m, 0, sizeof(m));
	m.msg_iov = iov;
	m.msg_iovlen = payload_bytes > 0 ? 2 : 1;
	if (handle_count > 0) {
		memset(&cbuf, 0, sizeof(cbuf));
		m.msg_control = cbuf.b;
		m.msg_controllen = CMSG_SPACE(sizeof(int) * handle_count);
		struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type = SCM_RIGHTS;
		cm->cmsg_len = CMSG_LEN(sizeof(int) * handle_count);
		int *out = (int *)CMSG_DATA(cm);
		for (uint32_t i = 0; i < handle_count; i++) {
			out[i] = (int)handles[i];
		}
	}

	os_mutex_lock(&c->send_mtx);
	size_t total = sizeof(f) + payload_bytes;
	bool ok = !c->dead;
	bool first = true;
	while (ok && total > 0) {
#ifdef MSG_NOSIGNAL
		const ssize_t r = sendmsg(c->fd, &m, MSG_NOSIGNAL);
#else
		const ssize_t r = sendmsg(c->fd, &m, 0);
#endif
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r <= 0) {
			ok = false;
			break;
		}
		total -= (size_t)r;
		if (first) {
			// Descriptors ride the first chunk only.
			m.msg_control = NULL;
			m.msg_controllen = 0;
			first = false;
		}
		// Advance the iovecs past what was written.
		size_t adv = (size_t)r;
		for (size_t i = 0; i < m.msg_iovlen && adv > 0; i++) {
			const size_t take = adv < m.msg_iov[i].iov_len ? adv : m.msg_iov[i].iov_len;
			m.msg_iov[i].iov_base = (uint8_t *)m.msg_iov[i].iov_base + take;
			m.msg_iov[i].iov_len -= take;
			adv -= take;
		}
	}
	os_mutex_unlock(&c->send_mtx);
	return ok;
}

void
conv_host_chan_shutdown(struct conv_host_chan *c)
{
	if (c == NULL) {
		return;
	}
	c->dead = 1;
	(void)shutdown(c->fd, SHUT_RDWR);
}

void
conv_host_chan_destroy(struct conv_host_chan **c_ptr)
{
	if (c_ptr == NULL || *c_ptr == NULL) {
		return;
	}
	struct conv_host_chan *c = *c_ptr;
	close(c->fd);
	os_mutex_destroy(&c->send_mtx);
	free(c);
	*c_ptr = NULL;
}

void
conv_host_handle_close(uint64_t h)
{
	const int fd = (int)h;
	if (fd > 2) {
		close(fd);
	}
}

bool
conv_host_wake_wait(uint64_t h, uint32_t timeout_ms)
{
	struct pollfd p = {(int)h, POLLIN, 0};
	int r;
	do {
		r = poll(&p, 1, (int)timeout_ms);
	} while (r < 0 && errno == EINTR);
	if (r <= 0) {
		return false;
	}
	// Drain: an eventfd returns its 8-byte counter, a pipe / socket its bytes.
	uint8_t buf[64];
	const int fl = fcntl((int)h, F_GETFL);
	(void)fcntl((int)h, F_SETFL, fl | O_NONBLOCK);
	while (read((int)h, buf, sizeof(buf)) > 0) {
	}
	(void)fcntl((int)h, F_SETFL, fl);
	return true;
}

void
conv_host_wake_signal(uint64_t h)
{
	const uint64_t one = 1;
	ssize_t r;
	do {
		r = write((int)h, &one, sizeof(one));
	} while (r < 0 && errno == EINTR);
}

void *
conv_host_shm_map(uint64_t h, uint64_t bytes)
{
	void *p = mmap(NULL, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, (int)h, 0);
	return p == MAP_FAILED ? NULL : p;
}

void
conv_host_shm_unmap(void *p, uint64_t bytes)
{
	if (p != NULL) {
		munmap(p, (size_t)bytes);
	}
}

uint64_t
conv_host_shm_create(uint64_t bytes, void **out_map)
{
	char name[64];
	static unsigned counter = 0;
	(void)snprintf(name, sizeof(name), "/dxr-convhost-%ld-%u", (long)getpid(), __sync_add_and_fetch(&counter, 1));
	const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		return 0;
	}
	(void)shm_unlink(name);
	if (ftruncate(fd, (off_t)bytes) != 0) {
		close(fd);
		return 0;
	}
	void *m = mmap(NULL, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) {
		close(fd);
		return 0;
	}
	*out_map = m;
	return (uint64_t)fd;
}

#endif
