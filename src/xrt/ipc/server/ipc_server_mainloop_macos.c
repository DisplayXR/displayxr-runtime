// Copyright 2020-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Server mainloop details on macOS, using kqueue.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author David Fattal <david.fattal@leiainc.com>
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
#include <sys/event.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <limits.h>

/*
 * "XRT_NO_STDIN" option disables stdin and prevents monado-service from terminating.
 * This could be useful for situations where there is no proper or in a non-interactive shell.
 */
DEBUG_GET_ONCE_BOOL_OPTION(skip_stdin, "XRT_NO_STDIN", false)


/*
 *
 * Static functions.
 *
 */

static int
create_listen_socket(struct ipc_server_mainloop *ml, int *out_fd)
{
	struct sockaddr_un addr;
	int fd;
	int ret;

	fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		U_LOG_E("Message Socket Create Error!");
		return fd;
	}

	// Set SO_NOSIGPIPE to prevent SIGPIPE on broken connections (macOS equivalent of MSG_NOSIGNAL).
	int on = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));

	char sock_file[PATH_MAX];

	int size = u_file_get_path_in_runtime_dir(XRT_IPC_MSG_SOCK_FILENAME, sock_file, PATH_MAX);
	if (size == -1) {
		U_LOG_E("Could not get socket file name");
		close(fd);
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, sock_file);

	ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));

	// On macOS, if a stale socket file exists, try to remove it and rebind.
	if (ret < 0 && errno == EADDRINUSE) {
		U_LOG_W("Removing stale socket file %s", sock_file);

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

	ret = create_listen_socket(ml, &fd);
	if (ret < 0) {
		return ret;
	}

	ml->listen_socket = fd;
	U_LOG_D("Listening socket is fd %d", ml->listen_socket);

	return fd;
}

static int
init_kqueue(struct ipc_server_mainloop *ml)
{
	int ret = kqueue();
	if (ret < 0) {
		U_LOG_E("kqueue() failed: '%s'", strerror(errno));
		return ret;
	}

	ml->kqueue_fd = ret;

	struct kevent changes[2];
	int nchanges = 0;

	// Monitor stdin for shutdown (like the Linux epoll path).
	// Skip if stdin is not a TTY (background process) or XRT_NO_STDIN is set.
	if (!debug_get_bool_option_skip_stdin() && isatty(0)) {
		EV_SET(&changes[nchanges], 0 /* stdin */, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
		nchanges++;
	}

	// Monitor the listen socket for new connections.
	EV_SET(&changes[nchanges], ml->listen_socket, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
	nchanges++;

	ret = kevent(ml->kqueue_fd, changes, nchanges, NULL, 0, NULL);
	if (ret < 0) {
		U_LOG_E("kevent() registration failed: '%s'", strerror(errno));
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

	// Set SO_NOSIGPIPE on the accepted client socket too.
	int on = 1;
	setsockopt(ret, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));

	// Call into the generic client connected handling code.
	ipc_server_handle_client_connected(vs, ret);
}

#define NUM_POLL_EVENTS 8
#define NO_SLEEP_SEC 0
#define NO_SLEEP_NSEC 0


/*
 *
 * Exported functions
 *
 */

void
ipc_server_mainloop_poll(struct ipc_server *vs, struct ipc_server_mainloop *ml)
{
	IPC_TRACE_MARKER();

	struct kevent events[NUM_POLL_EVENTS];
	struct timespec timeout = {NO_SLEEP_SEC, NO_SLEEP_NSEC};

	// No sleeping, returns immediately.
	int ret = kevent(ml->kqueue_fd, NULL, 0, events, NUM_POLL_EVENTS, &timeout);
	if (ret < 0) {
		U_LOG_E("kevent() poll failed: '%s'", strerror(errno));
		ipc_server_handle_failure(vs);
		return;
	}

	for (int i = 0; i < ret; i++) {
		int fd = (int)events[i].ident;

		// If we get data on stdin, stop.
		if (fd == 0) {
			ipc_server_handle_shutdown_signal(vs);
			return;
		}

		// Somebody new at the door.
		if (fd == ml->listen_socket) {
			handle_listen(vs, ml);
		}
	}
}

int
ipc_server_mainloop_init(struct ipc_server_mainloop *ml)
{
	IPC_TRACE_MARKER();

	int ret = init_listen_socket(ml);
	if (ret < 0) {
		ipc_server_mainloop_deinit(ml);
		return ret;
	}

	ret = init_kqueue(ml);
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

	if (ml->kqueue_fd >= 0) {
		close(ml->kqueue_fd);
		ml->kqueue_fd = -1;
	}

	if (ml->listen_socket > 0) {
		close(ml->listen_socket);
		ml->listen_socket = -1;
		if (ml->socket_filename) {
			unlink(ml->socket_filename);
			free(ml->socket_filename);
			ml->socket_filename = NULL;
		}
	}
}
