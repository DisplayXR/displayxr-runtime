// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  os_process.h on Linux (desktop) and macOS: fork + exec,
 *         PR_SET_PDEATHSIG / kqueue NOTE_EXIT, AF_UNIX socketpair +
 *         SCM_RIGHTS, a socketpair byte as the wake.
 * @ingroup aux_os
 */

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE // MSG_CMSG_CLOEXEC, SOCK_CLOEXEC
#endif

#include "os/os_process.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif
#if defined(__APPLE__)
#include <sys/event.h>
#endif

//! Most descriptors one message may carry.
#define MAX_FDS 16


/*
 *
 * Helpers.
 *
 */

static int
fd_of(uint64_t h)
{
	return h == OS_NATIVE_HANDLE_INVALID ? -1 : (int)(int64_t)h;
}

static uint64_t
from_fd(int fd)
{
	return fd < 0 ? OS_NATIVE_HANDLE_INVALID : (uint64_t)(uint32_t)fd;
}

static void
set_cloexec(int fd)
{
	int f = fcntl(fd, F_GETFD);
	if (f >= 0) {
		(void)fcntl(fd, F_SETFD, f | FD_CLOEXEC);
	}
}

static void
set_nonblock(int fd)
{
	int f = fcntl(fd, F_GETFL);
	if (f >= 0) {
		(void)fcntl(fd, F_SETFL, f | O_NONBLOCK);
	}
}

static void
set_nosigpipe(int fd)
{
#if defined(SO_NOSIGPIPE)
	int one = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
	(void)fd;
#endif
}

#if defined(MSG_NOSIGNAL)
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

//! A CLOEXEC AF_UNIX stream socketpair.
static int
make_socketpair(int sv[2])
{
#if defined(SOCK_CLOEXEC)
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
		return -1;
	}
#else
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		return -1;
	}
	set_cloexec(sv[0]);
	set_cloexec(sv[1]);
#endif
	set_nosigpipe(sv[0]);
	set_nosigpipe(sv[1]);
	return 0;
}

static uint64_t
mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

//! poll() one fd with a timeout that survives EINTR. Returns poll's result.
static int
poll_one(int fd, short events, uint32_t timeout_ms, short *out_revents)
{
	uint64_t start = mono_ms();
	for (;;) {
		int t = -1;
		if (timeout_ms != OS_PROCESS_WAIT_INFINITE) {
			uint64_t elapsed = mono_ms() - start;
			t = elapsed >= timeout_ms ? 0 : (int)(timeout_ms - elapsed);
		}
		struct pollfd p = {fd, events, 0};
		int r = poll(&p, 1, t);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		*out_revents = p.revents;
		return r;
	}
}


/*
 *
 * Processes.
 *
 */

struct os_process
{
	pid_t pid;
	bool exited;
	bool terminated; //!< os_process_terminate sent SIGKILL
	int32_t terminate_code;
	int32_t exit_code;
};

int
os_process_spawn(const struct os_process_spawn_info *info, struct os_process **out_proc)
{
	if (info == NULL || info->exe_path == NULL || out_proc == NULL || (info->arg_count > 0 && info->args == NULL)) {
		return OS_PROCESS_ERR_INVALID;
	}
	*out_proc = NULL;

	// Everything the child needs is prepared BEFORE fork: between fork and
	// exec only async-signal-safe calls are made.
	char **argv = (char **)calloc((size_t)info->arg_count + 2, sizeof(char *));
	struct os_process *p = (struct os_process *)calloc(1, sizeof(*p));
	if (argv == NULL || p == NULL) {
		free(argv);
		free(p);
		return OS_PROCESS_ERR_FAILED;
	}
	argv[0] = (char *)info->exe_path;
	for (uint32_t i = 0; i < info->arg_count; i++) {
		argv[i + 1] = (char *)(info->args[i] != NULL ? info->args[i] : "");
	}

	int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
	int log = -1;
	if (info->log_path != NULL) {
		log = open(info->log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	}
	int ch = fd_of(info->channel_child_handle);
	long max_fd = sysconf(_SC_OPEN_MAX);
	if (max_fd < 0 || max_fd > 65536) {
		max_fd = 65536;
	}
#if defined(__linux__)
	pid_t parent = getpid();
#endif

	pid_t pid = fork();
	if (pid < 0) {
		if (devnull >= 0) {
			close(devnull);
		}
		if (log >= 0) {
			close(log);
		}
		free(argv);
		free(p);
		return OS_PROCESS_ERR_FAILED;
	}

	if (pid == 0) {
		// Child.
#if defined(__linux__)
		if (info->kill_on_parent_exit) {
			if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent) {
				_exit(127);
			}
		}
#endif
		// Move the channel out of the way of 0..3 first.
		int ch_hi = -1;
		if (ch >= 0) {
			ch_hi = fcntl(ch, F_DUPFD, 10);
			if (ch_hi < 0) {
				_exit(127);
			}
		}
		if (devnull >= 0) {
			dup2(devnull, 0);
			if (log < 0) {
				dup2(devnull, 1);
				dup2(devnull, 2);
			}
		}
		if (log >= 0) {
			dup2(log, 1);
			dup2(log, 2);
		}
		if (ch_hi >= 0) {
			if (dup2(ch_hi, OS_CHANNEL_CHILD_FD) < 0) {
				_exit(127);
			}
		}
		// Nothing else leaks into the child.
		int first_closed = ch_hi >= 0 ? OS_CHANNEL_CHILD_FD + 1 : 3;
		for (long fd = first_closed; fd < max_fd; fd++) {
			close((int)fd);
		}
		execv(info->exe_path, argv);
		_exit(127);
	}

	// Parent.
	if (devnull >= 0) {
		close(devnull);
	}
	if (log >= 0) {
		close(log);
	}
	free(argv);
	p->pid = pid;
	*out_proc = p;
	return 0;
}

long
os_process_pid(const struct os_process *proc)
{
	return proc != NULL ? (long)proc->pid : 0;
}

static void
record_status(struct os_process *p, int status)
{
	p->exited = true;
	if (WIFEXITED(status)) {
		p->exit_code = WEXITSTATUS(status);
	} else if (WIFSIGNALED(status)) {
		int sig = WTERMSIG(status);
		if (p->terminated && sig == SIGKILL) {
			p->exit_code = p->terminate_code;
		} else {
			p->exit_code = OS_PROCESS_EXIT_SIGNAL_BASE + sig;
		}
	} else {
		p->exit_code = -1;
	}
}

int
os_process_wait(struct os_process *proc, uint32_t timeout_ms, int32_t *out_exit_code)
{
	if (proc == NULL) {
		return OS_PROCESS_ERR_INVALID;
	}
	uint64_t start = mono_ms();
	uint32_t sleep_us = 500;
	while (!proc->exited) {
		int status = 0;
		pid_t r = waitpid(proc->pid, &status, WNOHANG);
		if (r == proc->pid) {
			record_status(proc, status);
			break;
		}
		if (r < 0 && errno != EINTR) {
			return OS_PROCESS_ERR_FAILED;
		}
		if (timeout_ms != OS_PROCESS_WAIT_INFINITE && mono_ms() - start >= timeout_ms) {
			return 0;
		}
		struct timespec ts = {0, (long)sleep_us * 1000};
		nanosleep(&ts, NULL);
		if (sleep_us < 10000) {
			sleep_us *= 2;
		}
	}
	if (out_exit_code != NULL) {
		*out_exit_code = proc->exit_code;
	}
	return 1;
}

int
os_process_terminate(struct os_process *proc, int32_t exit_code)
{
	if (proc == NULL) {
		return OS_PROCESS_ERR_INVALID;
	}
	if (proc->exited) {
		return 0;
	}
	if (!proc->terminated) {
		proc->terminated = true;
		proc->terminate_code = exit_code;
	}
	if (kill(proc->pid, SIGKILL) != 0 && errno != ESRCH) {
		return OS_PROCESS_ERR_FAILED;
	}
	return 0;
}

void
os_process_destroy(struct os_process **proc_ptr)
{
	if (proc_ptr == NULL || *proc_ptr == NULL) {
		return;
	}
	struct os_process *p = *proc_ptr;
	if (!p->exited) {
		int status = 0;
		if (waitpid(p->pid, &status, WNOHANG) == p->pid) {
			record_status(p, status);
		}
	}
	free(p);
	*proc_ptr = NULL;
}

int
os_process_dup_to_child(struct os_process *proc, uint64_t local, uint64_t *out_remote)
{
	(void)proc;
	(void)local;
	(void)out_remote;
	return OS_PROCESS_ERR_UNSUPPORTED;
}

int
os_process_dup_from_child(struct os_process *proc, uint64_t remote, bool close_source, uint64_t *out_local)
{
	(void)proc;
	(void)remote;
	(void)close_source;
	(void)out_local;
	return OS_PROCESS_ERR_UNSUPPORTED;
}

void
os_native_handle_close(uint64_t handle)
{
	int fd = fd_of(handle);
	if (fd >= 0) {
		close(fd);
	}
}


/*
 *
 * Child-side helpers.
 *
 */

#if defined(__APPLE__)
struct parent_watch
{
	int kq;
	int32_t exit_code;
};

static void *
parent_watch_thread(void *arg)
{
	struct parent_watch *w = (struct parent_watch *)arg;
	for (;;) {
		struct kevent ev;
		int n = kevent(w->kq, NULL, 0, &ev, 1, NULL);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n > 0 && ev.filter == EVFILT_PROC && (ev.fflags & NOTE_EXIT) != 0) {
			_exit(w->exit_code);
		}
		if (n < 0) {
			return NULL;
		}
	}
}
#endif

int
os_process_watch_parent(long parent_pid, int32_t exit_code)
{
	if (parent_pid <= 0) {
		return OS_PROCESS_ERR_INVALID;
	}
#if defined(__linux__)
	if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
		return OS_PROCESS_ERR_FAILED;
	}
	if ((long)getppid() != parent_pid) {
		_exit(exit_code); // the parent died before the signal was armed
	}
	return 0;
#elif defined(__APPLE__)
	struct parent_watch *w = (struct parent_watch *)calloc(1, sizeof(*w));
	if (w == NULL) {
		return OS_PROCESS_ERR_FAILED;
	}
	w->exit_code = exit_code;
	w->kq = kqueue();
	if (w->kq < 0) {
		free(w);
		return OS_PROCESS_ERR_FAILED;
	}
	set_cloexec(w->kq);
	struct kevent ev;
	EV_SET(&ev, (uintptr_t)parent_pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
	if (kevent(w->kq, &ev, 1, NULL, 0, NULL) != 0) {
		if (errno == ESRCH) {
			_exit(exit_code); // already gone
		}
		close(w->kq);
		free(w);
		return OS_PROCESS_ERR_FAILED;
	}
	if ((long)getppid() != parent_pid) {
		_exit(exit_code);
	}
	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	int r = pthread_create(&th, &attr, parent_watch_thread, w);
	pthread_attr_destroy(&attr);
	if (r != 0) {
		close(w->kq);
		free(w);
		return OS_PROCESS_ERR_FAILED;
	}
	return 0; // w lives as long as the thread, i.e. the process
#else
	(void)exit_code;
	return OS_PROCESS_ERR_UNSUPPORTED;
#endif
}

bool
os_process_is_elevated(void)
{
	return geteuid() == 0;
}

long
os_process_current_pid(void)
{
	return (long)getpid();
}

bool
os_named_mutex_held(const char *name)
{
	(void)name;
	return false;
}


/*
 *
 * Channel: one AF_UNIX stream socket end.
 *
 */

struct os_channel
{
	int fd;
};

int
os_channel_create_for_child(const char *name_hint,
                            struct os_channel **out_parent,
                            char *out_token,
                            size_t token_size,
                            uint64_t *out_child_handle)
{
	(void)name_hint;
	if (out_parent == NULL || out_token == NULL || out_child_handle == NULL || token_size < 4) {
		return OS_PROCESS_ERR_INVALID;
	}
	*out_parent = NULL;
	*out_child_handle = OS_NATIVE_HANDLE_INVALID;

	int sv[2];
	if (make_socketpair(sv) != 0) {
		return OS_PROCESS_ERR_FAILED;
	}
	struct os_channel *c = (struct os_channel *)calloc(1, sizeof(*c));
	if (c == NULL) {
		close(sv[0]);
		close(sv[1]);
		return OS_PROCESS_ERR_FAILED;
	}
	c->fd = sv[0];
	snprintf(out_token, token_size, "%d", OS_CHANNEL_CHILD_FD);
	*out_child_handle = from_fd(sv[1]);
	*out_parent = c;
	return 0;
}

int
os_channel_accept_child(struct os_channel *parent,
                        const struct os_process *child,
                        uint64_t child_handle,
                        uint32_t timeout_ms)
{
	(void)child;
	(void)timeout_ms;
	if (parent == NULL) {
		return OS_PROCESS_ERR_INVALID;
	}
	// The child has its own copy (fd OS_CHANNEL_CHILD_FD); ours must go, or
	// the parent would never see the child's end close.
	os_native_handle_close(child_handle);
	return 0;
}

int
os_channel_open_child(const char *token, struct os_channel **out_channel)
{
	if (token == NULL || out_channel == NULL) {
		return OS_PROCESS_ERR_INVALID;
	}
	*out_channel = NULL;
	char *end = NULL;
	long fd = strtol(token, &end, 10);
	if (end == token || *end != '\0' || fd < 0 || fd > 65535) {
		return OS_PROCESS_ERR_INVALID;
	}
	if (fcntl((int)fd, F_GETFD) < 0) {
		return OS_PROCESS_ERR_FAILED;
	}
	set_cloexec((int)fd);
	set_nosigpipe((int)fd);
	struct os_channel *c = (struct os_channel *)calloc(1, sizeof(*c));
	if (c == NULL) {
		return OS_PROCESS_ERR_FAILED;
	}
	c->fd = (int)fd;
	*out_channel = c;
	return 0;
}

int
os_channel_send(struct os_channel *ch, const void *buf, size_t len, const uint64_t *handles, uint32_t handle_count)
{
	if (ch == NULL || (buf == NULL && len > 0) || handle_count > MAX_FDS || (handle_count > 0 && handles == NULL) ||
	    (handle_count > 0 && len == 0)) {
		return OS_PROCESS_ERR_INVALID;
	}
	const uint8_t *p = (const uint8_t *)buf;
	bool fds_sent = handle_count == 0;

	while (len > 0) {
		struct iovec iov = {(void *)p, len};
		struct msghdr msg;
		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;

		union {
			struct cmsghdr align;
			char buf[CMSG_SPACE(sizeof(int) * MAX_FDS)];
		} cbuf;
		if (!fds_sent) {
			memset(&cbuf, 0, sizeof(cbuf));
			msg.msg_control = cbuf.buf;
			msg.msg_controllen = CMSG_SPACE(sizeof(int) * handle_count);
			struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
			cm->cmsg_level = SOL_SOCKET;
			cm->cmsg_type = SCM_RIGHTS;
			cm->cmsg_len = CMSG_LEN(sizeof(int) * handle_count);
			int fds[MAX_FDS];
			for (uint32_t i = 0; i < handle_count; i++) {
				fds[i] = fd_of(handles[i]);
				if (fds[i] < 0) {
					return OS_PROCESS_ERR_INVALID;
				}
			}
			memcpy(CMSG_DATA(cm), fds, sizeof(int) * handle_count);
		}

		ssize_t n = sendmsg(ch->fd, &msg, SEND_FLAGS);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return (errno == EPIPE || errno == ECONNRESET) ? OS_PROCESS_ERR_CLOSED : OS_PROCESS_ERR_FAILED;
		}
		fds_sent = true;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

int
os_channel_recv(struct os_channel *ch,
                void *buf,
                size_t len,
                uint64_t *handles,
                uint32_t *inout_handle_count,
                uint32_t timeout_ms)
{
	uint32_t cap = inout_handle_count != NULL ? *inout_handle_count : 0;
	uint32_t count = 0;
	if (inout_handle_count != NULL) {
		*inout_handle_count = 0;
	}
	if (ch == NULL || (buf == NULL && len > 0) || (cap > 0 && handles == NULL)) {
		return OS_PROCESS_ERR_INVALID;
	}

	uint8_t *p = (uint8_t *)buf;
	size_t got = 0;
	int ret = 1;
	while (got < len) {
		short rev = 0;
		int pr = poll_one(ch->fd, POLLIN, timeout_ms, &rev);
		if (pr < 0) {
			ret = OS_PROCESS_ERR_FAILED;
			break;
		}
		if (pr == 0) {
			ret = got == 0 ? 0 : OS_PROCESS_ERR_TIMEOUT;
			break;
		}

		struct iovec iov = {p + got, len - got};
		union {
			struct cmsghdr align;
			char buf[CMSG_SPACE(sizeof(int) * MAX_FDS)];
		} cbuf;
		struct msghdr msg;
		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cbuf.buf;
		msg.msg_controllen = sizeof(cbuf.buf);
		int flags = 0;
#if defined(MSG_CMSG_CLOEXEC)
		flags |= MSG_CMSG_CLOEXEC;
#endif
		ssize_t n = recvmsg(ch->fd, &msg, flags);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN) {
				continue;
			}
			ret = errno == ECONNRESET ? OS_PROCESS_ERR_CLOSED : OS_PROCESS_ERR_FAILED;
			break;
		}

		for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm != NULL; cm = CMSG_NXTHDR(&msg, cm)) {
			if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) {
				continue;
			}
			size_t nfd = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			int fds[MAX_FDS];
			if (nfd > MAX_FDS) {
				nfd = MAX_FDS;
			}
			memcpy(fds, CMSG_DATA(cm), nfd * sizeof(int));
			for (size_t i = 0; i < nfd; i++) {
				set_cloexec(fds[i]);
				if (count < cap) {
					handles[count++] = from_fd(fds[i]);
				} else {
					close(fds[i]); // no room: never leak
				}
			}
		}

		if (n == 0) {
			ret = OS_PROCESS_ERR_CLOSED;
			break;
		}
		got += (size_t)n;
	}

	if (ret != 1) {
		// Descriptors of an incomplete message are not handed out.
		for (uint32_t i = 0; i < count; i++) {
			os_native_handle_close(handles[i]);
			handles[i] = OS_NATIVE_HANDLE_INVALID;
		}
		count = 0;
	}
	if (inout_handle_count != NULL) {
		*inout_handle_count = count;
	}
	return ret;
}

void
os_channel_destroy(struct os_channel **ch_ptr)
{
	if (ch_ptr == NULL || *ch_ptr == NULL) {
		return;
	}
	if ((*ch_ptr)->fd >= 0) {
		close((*ch_ptr)->fd);
	}
	free(*ch_ptr);
	*ch_ptr = NULL;
}


/*
 *
 * Wake: a byte on a nonblocking socketpair.
 *
 */

int
os_wake_create_pair(struct os_wake *local, uint64_t *out_remote_native)
{
	if (local == NULL || out_remote_native == NULL) {
		return OS_PROCESS_ERR_INVALID;
	}
	local->native = OS_NATIVE_HANDLE_INVALID;
	*out_remote_native = OS_NATIVE_HANDLE_INVALID;
	int sv[2];
	if (make_socketpair(sv) != 0) {
		return OS_PROCESS_ERR_FAILED;
	}
	set_nonblock(sv[0]);
	set_nonblock(sv[1]);
	local->native = from_fd(sv[0]);
	*out_remote_native = from_fd(sv[1]);
	return 0;
}

void
os_wake_from_native(struct os_wake *w, uint64_t native)
{
	w->native = native;
	int fd = fd_of(native);
	if (fd >= 0) {
		set_nonblock(fd);
		set_nosigpipe(fd);
	}
}

int
os_wake_signal(struct os_wake *w)
{
	int fd = w != NULL ? fd_of(w->native) : -1;
	if (fd < 0) {
		return OS_PROCESS_ERR_INVALID;
	}
	const uint8_t one = 1;
	for (;;) {
		ssize_t n = send(fd, &one, 1, SEND_FLAGS);
		if (n == 1) {
			return 0;
		}
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			return 0; // the buffer is full of unread wakes: already signalled
		}
		return (n < 0 && (errno == EPIPE || errno == ECONNRESET)) ? OS_PROCESS_ERR_CLOSED
		                                                           : OS_PROCESS_ERR_FAILED;
	}
}

int
os_wake_wait(struct os_wake *w, uint32_t timeout_ms)
{
	int fd = w != NULL ? fd_of(w->native) : -1;
	if (fd < 0) {
		return OS_PROCESS_ERR_INVALID;
	}
	short rev = 0;
	int pr = poll_one(fd, POLLIN, timeout_ms, &rev);
	if (pr < 0) {
		return OS_PROCESS_ERR_FAILED;
	}
	if (pr == 0) {
		return 0;
	}
	// Drain every pending wake (they coalesce).
	bool got = false;
	for (;;) {
		uint8_t sink[64];
		ssize_t n = recv(fd, sink, sizeof(sink), 0);
		if (n > 0) {
			got = true;
			continue;
		}
		if (n == 0) {
			return got ? 1 : OS_PROCESS_ERR_CLOSED;
		}
		if (errno == EINTR) {
			continue;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return got ? 1 : 0;
		}
		return OS_PROCESS_ERR_FAILED;
	}
}

void
os_wake_destroy(struct os_wake *w)
{
	if (w == NULL) {
		return;
	}
	os_native_handle_close(w->native);
	w->native = OS_NATIVE_HANDLE_INVALID;
}
