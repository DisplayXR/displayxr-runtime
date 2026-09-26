// Copyright 2020-2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Server mainloop details on Linux.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup ipc_server
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_config_have.h"
#include "xrt/xrt_config_os.h"

#include "os/os_time.h"
#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_debug.h"
#include "util/u_trace_marker.h"
#include "util/u_file.h"

#include "shared/ipc_shmem.h"
#include "server/ipc_server.h"

#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include "util/u_debug.h"

/*
 * "XRT_NO_STDIN" option disables stdin and prevents displayxr-service from terminating.
 * This could be useful for situations where there is no proper or in a non-interactive shell.
 * Two example scenarios are:
 *    * IDE terminals,
 *    * Some scripting environments where displayxr-service is spawned in the background
 */
DEBUG_GET_ONCE_BOOL_OPTION(skip_stdin, "XRT_NO_STDIN", false)

/*
 *
 * Static functions.
 *
 */
/*!
 * The sd_listen_fds(3) protocol without libsystemd (#1744).
 *
 * The service does not link libsystemd (the .deb's STABLE_SONAMES gate would
 * refuse it, and one getenv pair does not justify a new Depends), but the .deb
 * DOES socket-activate it from a systemd user unit. The protocol is
 * three environment variables: LISTEN_PID names the process the fds are meant
 * for (so a child that inherits the env does not also claim them), LISTEN_FDS
 * counts them, and they start at fd 3. Returns the fd count, 0 for "not socket
 * activated", <0 on a malformed hand-off.
 */
static int
listen_fds_from_env(void)
{
	const char *pid_str = getenv("LISTEN_PID");
	const char *fds_str = getenv("LISTEN_FDS");
	if (pid_str == NULL || fds_str == NULL) {
		return 0;
	}

	char *end = NULL;
	errno = 0;
	long pid = strtol(pid_str, &end, 10);
	if (errno != 0 || end == pid_str || *end != '\0' || pid != (long)getpid()) {
		return 0; // Meant for another process (our parent, typically).
	}
	errno = 0;
	long n = strtol(fds_str, &end, 10);
	if (errno != 0 || end == fds_str || *end != '\0' || n < 0 || n > INT_MAX - 3) {
		return -1;
	}

	// Consumed: do not leak the hand-off into anything we spawn.
	unsetenv("LISTEN_PID");
	unsetenv("LISTEN_FDS");
	unsetenv("LISTEN_FDNAMES");

	for (int fd = 3; fd < 3 + (int)n; fd++) {
		int flags = fcntl(fd, F_GETFD);
		if (flags < 0) {
			return -1;
		}
		(void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
	}
	return (int)n;
}

//! First fd of a socket-activation hand-off (SD_LISTEN_FDS_START).
#define DXR_LISTEN_FDS_START 3

static int
get_systemd_socket(struct ipc_server_mainloop *ml, int *out_fd)
{
	// We may have been launched with socket activation
	int num_fds = listen_fds_from_env();
	const int first_fd = DXR_LISTEN_FDS_START;
	if (num_fds < 0) {
		U_LOG_E("Malformed socket-activation hand-off (LISTEN_FDS).");
		return -1;
	}
	if (num_fds > 1) {
		U_LOG_E("Too many file descriptors passed by systemd.");
		return -1;
	}
	if (num_fds == 1) {
		struct stat st;
		if (fstat(first_fd, &st) != 0 || !S_ISSOCK(st.st_mode)) {
			U_LOG_E("Socket activation passed fd %d, which is not a socket.", first_fd);
			return -1;
		}
		*out_fd = first_fd;
		ml->launched_by_socket = true;
		// Lifecycle, once: WARN so a release log shows how the service started.
		U_LOG_W("Listening on the socket-activated fd %d (systemd).", first_fd);
	}
	return 0;
}

/*!
 * Is a service already answering on @p sock_file? A connect() is the only
 * honest liveness test for an AF_UNIX path: a stale file left by a service that
 * was killed (SIGKILL, OOM, a crash — anything that skipped the unlink in
 * deinit) refuses the connection, a live one accepts it.
 */
static bool
socket_path_is_live(const char *sock_file)
{
	// The caller reports the bind() failure from errno afterwards: keep it.
	const int saved_errno = errno;
	int fd = socket(PF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		errno = saved_errno;
		return true; // Cannot tell; do not delete what might be live.
	}
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_file);
	int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	int err = errno;
	close(fd);
	errno = saved_errno;
	return ret == 0 || (err != ECONNREFUSED && err != ENOENT);
}

static int
create_listen_socket(struct ipc_server_mainloop *ml, int *out_fd)
{
	// no fd provided
	struct sockaddr_un addr;
	int fd;
	int ret;

	fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		U_LOG_E("Message Socket Create Error!");
		return fd;
	}


	char sock_file[PATH_MAX];

	int size = u_file_get_path_in_runtime_dir(XRT_IPC_MSG_SOCK_FILENAME, sock_file, PATH_MAX);
	if (size == -1) {
		U_LOG_E("Could not get socket file name");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));

	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, sock_file);

	ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));

	// A socket file nobody answers on is left over from a service that died
	// without unlinking it. Without this, one crash (or a SIGKILL) wedges every
	// later start — outside socket activation — until someone deletes the file
	// by hand. (Monado gated this on a libbsd pidfile; the connect probe needs
	// no extra dependency and is the more direct test.)
	if (ret < 0 && errno == EADDRINUSE && !socket_path_is_live(sock_file)) {
		U_LOG_W("Removing stale socket file %s (nothing is listening on it)", sock_file);

		ret = unlink(sock_file);
		if (ret < 0) {
			U_LOG_E("Failed to remove stale socket file %s: %s", sock_file, strerror(errno));
			close(fd);
			return ret;
		}
		ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
	}

	if (ret < 0) {
		U_LOG_E("Could not bind socket to path %s: %s. Is the service running already?", sock_file,
		        strerror(errno));
		U_LOG_E(
		    "Or, is the systemd user unit displayxr.socket active? (systemctl --user status displayxr.socket)");
		if (errno == EADDRINUSE) {
			U_LOG_E("If displayxr-service is not running, delete %s before starting a new instance",
			        sock_file);
		}
		close(fd);
		return ret;
	}
	// Save for later
	ml->socket_filename = strdup(sock_file);

	ret = listen(fd, IPC_MAX_CLIENTS);
	if (ret < 0) {
		close(fd);
		return ret;
	}
	U_LOG_D("Created listening socket %s.", sock_file);
	*out_fd = fd;
	return 0;
}

static int
init_listen_socket(struct ipc_server_mainloop *ml)
{
	int fd = -1;
	int ret;
	ml->listen_socket = -1;

	ret = get_systemd_socket(ml, &fd);
	if (ret < 0) {
		return ret;
	}

	if (fd == -1) {
		ret = create_listen_socket(ml, &fd);
		if (ret < 0) {
			return ret;
		}
	}
	// All ok!
	ml->listen_socket = fd;
	U_LOG_D("Listening socket is fd %d", ml->listen_socket);

	return fd;
}

/*!
 * Write end of the self-pipe the shutdown-signal handler pokes. A plain
 * write(2) is async-signal-safe, and the handler runs on whichever thread the
 * kernel picks — the service has many by now — so it must not touch anything
 * but this fd.
 */
static int s_signal_pipe_write = -1;

static void
signal_handler(int sig)
{
	(void)sig;
	int saved = errno;
	if (s_signal_pipe_write >= 0) {
		char c = 1;
		ssize_t r = write(s_signal_pipe_write, &c, 1);
		(void)r; // A full pipe already carries a pending stop.
	}
	errno = saved;
}

static int
init_signal_pipe(struct ipc_server_mainloop *ml)
{
	ml->signal_pipe[0] = -1;
	ml->signal_pipe[1] = -1;
	if (pipe2(ml->signal_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
		U_LOG_E("pipe2 for the shutdown-signal pipe failed: %s", strerror(errno));
		return -1;
	}
	ml->signal_pipe_valid = true;
	s_signal_pipe_write = ml->signal_pipe[1];

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	return 0;
}

static int
init_epoll(struct ipc_server_mainloop *ml)
{
	int ret = epoll_create1(EPOLL_CLOEXEC);
	if (ret < 0) {
		return ret;
	}

	ml->epoll_fd = ret;

	struct epoll_event ev = {0};

	// Shutdown signals (#1744): SIGTERM is how systemd, a desktop session and
	// `kill` stop a service; without a handler it killed the process outright,
	// skipping deinit, so the bound socket file was never unlinked.
	if (ml->signal_pipe_valid) {
		ev.events = EPOLLIN;
		ev.data.fd = ml->signal_pipe[0];
		ret = epoll_ctl(ml->epoll_fd, EPOLL_CTL_ADD, ml->signal_pipe[0], &ev);
		if (ret < 0) {
			U_LOG_E("epoll_ctl(signal pipe) failed '%i'", ret);
			return ret;
		}
	}

	if (!ml->launched_by_socket && !debug_get_bool_option_skip_stdin()) {
		// Interactive convenience: a line (or EOF) on stdin stops the
		// service. Can't do this when launched by systemd socket activation.
		//
		// Best effort (#1744): a detached service has no terminal. Under
		// systemd, an XDG autostart entry or `nohup ... </dev/null`, stdin is
		// /dev/null, which epoll refuses with EPERM (regular files and
		// /dev/null are not pollable), or it is closed (EBADF). That used to
		// be fatal ("epoll_ctl(stdin) failed") and forced the
		// `sleep infinity | displayxr-service` workaround. Such a stdin simply
		// cannot carry the stop request, so run without it.
		ev.events = EPOLLIN;
		ev.data.fd = 0; // stdin
		ret = epoll_ctl(ml->epoll_fd, EPOLL_CTL_ADD, 0, &ev);
		if (ret < 0) {
			U_LOG_W("stdin is not pollable (%s) — running detached; stop with SIGTERM/SIGINT",
			        strerror(errno));
		}
	}

	ev.events = EPOLLIN;
	ev.data.fd = ml->listen_socket;
	ret = epoll_ctl(ml->epoll_fd, EPOLL_CTL_ADD, ml->listen_socket, &ev);
	if (ret < 0) {
		U_LOG_E("epoll_ctl(listen_socket) failed '%i'", ret);
		return ret;
	}

	return 0;
}

static void
handle_listen(struct ipc_server *vs, struct ipc_server_mainloop *ml)
{
	int ret = accept(ml->listen_socket, NULL, NULL);
	if (ret < 0) {
		U_LOG_E("accept '%i'", ret);
		ipc_server_handle_failure(vs);
		return;
	}

	// Call into the generic client connected handling code.
	ipc_server_handle_client_connected(vs, ret);
}

#define NUM_POLL_EVENTS 8
#define NO_SLEEP 0

/*
 *
 * Exported functions
 *
 */

void
ipc_server_mainloop_poll(struct ipc_server *vs, struct ipc_server_mainloop *ml)
{
	IPC_TRACE_MARKER();

	int epoll_fd = ml->epoll_fd;

	struct epoll_event events[NUM_POLL_EVENTS] = {0};

	// No sleeping, returns immediately.
	int ret = epoll_wait(epoll_fd, events, NUM_POLL_EVENTS, NO_SLEEP);
	if (ret < 0) {
		U_LOG_E("epoll_wait failed with '%i'.", ret);
		ipc_server_handle_failure(vs);
		return;
	}

	for (int i = 0; i < ret; i++) {
		// If we get data on stdin, stop.
		if (events[i].data.fd == 0) {
			ipc_server_handle_shutdown_signal(vs);
			return;
		}

		// SIGTERM / SIGINT (see signal_handler).
		if (ml->signal_pipe_valid && events[i].data.fd == ml->signal_pipe[0]) {
			U_LOG_W("Shutdown signal received — stopping the service");
			ipc_server_handle_shutdown_signal(vs);
			return;
		}

		// Somebody new at the door.
		if (events[i].data.fd == ml->listen_socket) {
			handle_listen(vs, ml);
		}
	}
}

int
ipc_server_mainloop_init(struct ipc_server_mainloop *ml)
{
	IPC_TRACE_MARKER();

	ml->signal_pipe_valid = false;

	int ret = init_listen_socket(ml);
	if (ret < 0) {
		ipc_server_mainloop_deinit(ml);
		return ret;
	}

	ret = init_signal_pipe(ml);
	if (ret < 0) {
		ipc_server_mainloop_deinit(ml);
		return ret;
	}

	ret = init_epoll(ml);
	if (ret < 0) {
		ipc_server_mainloop_deinit(ml);
		return ret;
	}
	return 0;
}

void
ipc_server_mainloop_deinit(struct ipc_server_mainloop *ml)
{
	IPC_TRACE_MARKER();

	if (ml == NULL) {
		return;
	}
	if (ml->listen_socket > 0) {
		// Close socket on exit
		close(ml->listen_socket);
		ml->listen_socket = -1;
		if (!ml->launched_by_socket && ml->socket_filename) {
			// Unlink it too, but only if we bound it.
			unlink(ml->socket_filename);
			free(ml->socket_filename);
			ml->socket_filename = NULL;
		}
	}
	if (ml->signal_pipe_valid) {
		// Back to the default action first, so a late signal cannot write
		// to a closed (or reused) fd.
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		s_signal_pipe_write = -1;
		close(ml->signal_pipe[1]);
		close(ml->signal_pipe[0]);
		ml->signal_pipe[0] = -1;
		ml->signal_pipe[1] = -1;
		ml->signal_pipe_valid = false;
	}
	//! @todo close epoll_fd?
}
