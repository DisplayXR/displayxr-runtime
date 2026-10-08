// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Lift-only service connection for in-process sessions (see
 *         ipc_client_lift_link.h, ADR-049).
 * @ingroup ipc_client
 */

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_instance.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include "client/ipc_client.h"
#include "client/ipc_client_connection.h"
#include "client/ipc_client_lift_link.h"

#ifdef XRT_OS_WINDOWS
#include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//! A failed connect is retried no more often than this.
#define LIFT_LINK_RETRY_NS (5ull * 1000 * 1000 * 1000)

struct ipc_client_lift_link
{
	struct os_mutex mtx;
	enum ipc_client_lift_link_state state;

	//! The current connection (CONNECTED), or the dead one awaiting its last
	//! release (DEAD). Owned.
	struct ipc_connection *conn;
	//! Holders of @ref conn (acquire without a release yet).
	uint32_t users;
	//! Bumped on every new connection.
	uint32_t gen;

	struct os_thread thread;
	bool thread_needs_join;
	//! The connect thread has finished (its result is in state/conn).
	bool connect_done;
	uint64_t retry_after_ns;

	//! One WARN per link for "no service", not one per retry.
	bool warned_unavailable;

	char label[XRT_MAX_APPLICATION_NAME_SIZE];
};

/*!
 * Is a service process alive in this session? The lift link must never LAUNCH
 * the service as a side effect of an app asking whether a conversion module
 * exists (the ordinary connect falls back to launching it), so it only dials
 * when the service's single-instance mutex can be opened.
 */
static bool
service_alive(void)
{
#ifdef XRT_OS_WINDOWS
	HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\DisplayXR.Service.Singleton");
	if (m == NULL) {
		return false;
	}
	CloseHandle(m);
	return true;
#else
	return true; // POSIX: no auto-launch on the plain socket connect.
#endif
}

static void *
connect_run(void *ptr)
{
	struct ipc_client_lift_link *link = (struct ipc_client_lift_link *)ptr;

	struct ipc_connection *c = NULL;
	xrt_result_t xret = XRT_ERROR_IPC_FAILURE;
	if (service_alive()) {
		c = U_TYPED_CALLOC(struct ipc_connection);
		if (c != NULL) {
			struct xrt_instance_info ii;
			U_ZERO(&ii);
			snprintf(ii.app_info.application_name, sizeof(ii.app_info.application_name), "%s", link->label);
			// An ordinary APP connection that never creates a session: the
			// service's lift handlers admit APP, and nothing else is asked of it.
			ii.app_info.declared_client_class = XRT_CLIENT_CLASS_APP;
			xret = ipc_client_connection_init(c, U_LOGGING_ERROR, &ii);
		}
	}

	os_mutex_lock(&link->mtx);
	if (xret == XRT_SUCCESS) {
		link->conn = c;
		link->gen++;
		link->state = IPC_CLIENT_LIFT_LINK_CONNECTED;
		link->warned_unavailable = false;
		U_LOG_W("[lift] in-process: lift-only service connection up (generation %u)", link->gen);
	} else {
		free(c); // init already finalised a half-made connection
		link->state = IPC_CLIENT_LIFT_LINK_FAILED;
		link->retry_after_ns = os_monotonic_get_ns() + LIFT_LINK_RETRY_NS;
		if (!link->warned_unavailable) {
			link->warned_unavailable = true;
			U_LOG_W(
			    "[lift] in-process: no service connection for 2D->3D conversion (%s) — lift reports "
			    "UNAVAILABLE; retrying every %u s",
			    c == NULL ? "service not running" : "connect refused or failed",
			    (unsigned)(LIFT_LINK_RETRY_NS / 1000000000ull));
		}
	}
	link->connect_done = true;
	os_mutex_unlock(&link->mtx);
	return NULL;
}

//! Caller holds mtx. Joins a finished connect thread.
static void
join_finished_locked(struct ipc_client_lift_link *link)
{
	if (link->thread_needs_join && link->connect_done) {
		// connect_done is set in the thread's LAST critical section, so it
		// never takes the mutex again: joining under it cannot deadlock, and
		// holding it keeps two callers from joining the same thread.
		os_thread_join(&link->thread);
		os_thread_destroy(&link->thread);
		link->thread_needs_join = false;
	}
}

//! Caller holds mtx. Closes a dead connection nobody holds any more.
static void
reap_dead_locked(struct ipc_client_lift_link *link)
{
	if (link->state == IPC_CLIENT_LIFT_LINK_DEAD && link->users == 0 && link->conn != NULL) {
		ipc_client_connection_fini(link->conn);
		free(link->conn);
		link->conn = NULL;
	}
}

//! Caller holds mtx. Starts a connect when none is in flight and one is due.
static void
maybe_start_connect_locked(struct ipc_client_lift_link *link)
{
	join_finished_locked(link);
	reap_dead_locked(link);

	bool due = false;
	switch (link->state) {
	case IPC_CLIENT_LIFT_LINK_CONNECTING: due = !link->thread_needs_join; break; // initial state
	case IPC_CLIENT_LIFT_LINK_FAILED: due = os_monotonic_get_ns() >= link->retry_after_ns; break;
	case IPC_CLIENT_LIFT_LINK_DEAD: due = link->conn == NULL; break; // old one closed
	case IPC_CLIENT_LIFT_LINK_CONNECTED: due = false; break;
	}
	if (!due || link->thread_needs_join) {
		return;
	}

	link->state = IPC_CLIENT_LIFT_LINK_CONNECTING;
	link->connect_done = false;
	if (os_thread_init(&link->thread) != 0 || os_thread_start(&link->thread, connect_run, link) != 0) {
		link->state = IPC_CLIENT_LIFT_LINK_FAILED;
		link->retry_after_ns = os_monotonic_get_ns() + LIFT_LINK_RETRY_NS;
		U_LOG_E("[lift] in-process: could not start the service connect thread");
		return;
	}
	link->thread_needs_join = true;
}

xrt_result_t
ipc_client_lift_link_create(const char *label, struct ipc_client_lift_link **out_link)
{
	struct ipc_client_lift_link *link = U_TYPED_CALLOC(struct ipc_client_lift_link);
	if (link == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	if (os_mutex_init(&link->mtx) != 0) {
		free(link);
		return XRT_ERROR_ALLOCATION;
	}
	link->state = IPC_CLIENT_LIFT_LINK_CONNECTING; // nothing in flight yet: first acquire starts it
	snprintf(link->label, sizeof(link->label), "%s", label != NULL ? label : "in-process lift");
	*out_link = link;
	return XRT_SUCCESS;
}

void
ipc_client_lift_link_destroy(struct ipc_client_lift_link **link_ptr)
{
	if (link_ptr == NULL || *link_ptr == NULL) {
		return;
	}
	struct ipc_client_lift_link *link = *link_ptr;
	*link_ptr = NULL;

	// An in-flight connect is bounded by the connect's own retry windows.
	if (link->thread_needs_join) {
		os_thread_join(&link->thread);
		os_thread_destroy(&link->thread);
		link->thread_needs_join = false;
	}
	if (link->conn != NULL) {
		// Closing the pipe is what tells the service to drop every stream
		// this connection owns (streams belong to the connection).
		ipc_client_connection_fini(link->conn);
		free(link->conn);
		link->conn = NULL;
	}
	os_mutex_destroy(&link->mtx);
	free(link);
}

struct ipc_connection *
ipc_client_lift_link_acquire(struct ipc_client_lift_link *link,
                             enum ipc_client_lift_link_state *out_state,
                             uint32_t *out_gen)
{
	return ipc_client_lift_link_acquire_wait(link, 0, out_state, out_gen);
}

struct ipc_connection *
ipc_client_lift_link_acquire_wait(struct ipc_client_lift_link *link,
                                  uint32_t timeout_ms,
                                  enum ipc_client_lift_link_state *out_state,
                                  uint32_t *out_gen)
{
	if (out_gen != NULL) {
		*out_gen = 0;
	}
	if (link == NULL) {
		if (out_state != NULL) {
			*out_state = IPC_CLIENT_LIFT_LINK_FAILED;
		}
		return NULL;
	}

	const uint64_t deadline = os_monotonic_get_ns() + (uint64_t)timeout_ms * 1000000ull;
	os_mutex_lock(&link->mtx);
	maybe_start_connect_locked(link);
	while (link->state == IPC_CLIENT_LIFT_LINK_CONNECTING && timeout_ms > 0 && os_monotonic_get_ns() < deadline) {
		os_mutex_unlock(&link->mtx);
		os_nanosleep(10 * 1000 * 1000);
		os_mutex_lock(&link->mtx);
	}

	struct ipc_connection *c = NULL;
	if (link->state == IPC_CLIENT_LIFT_LINK_CONNECTED && link->conn != NULL) {
		c = link->conn;
		link->users++;
		if (out_gen != NULL) {
			*out_gen = link->gen;
		}
	}
	if (out_state != NULL) {
		*out_state = link->state;
	}
	os_mutex_unlock(&link->mtx);
	return c;
}

void
ipc_client_lift_link_release(struct ipc_client_lift_link *link, uint32_t gen, bool ipc_failed)
{
	if (link == NULL) {
		return;
	}
	os_mutex_lock(&link->mtx);
	if (link->users > 0) {
		link->users--;
	}
	if (ipc_failed && gen == link->gen && link->state == IPC_CLIENT_LIFT_LINK_CONNECTED) {
		link->state = IPC_CLIENT_LIFT_LINK_DEAD;
		U_LOG_W(
		    "[lift] in-process: the lift service connection (generation %u) is gone — its streams are lost; "
		    "the session is unaffected and lift reconnects on the next call",
		    gen);
	}
	reap_dead_locked(link);
	os_mutex_unlock(&link->mtx);
}
