// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The in-process service link {lift, share} (see
 *         ipc_client_service_link.h; ADR-049 + Amendment 1, ADR-061 arm 2).
 * @ingroup ipc_client
 */

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_instance.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_share_link.h"

#include "client/ipc_client.h"
#include "client/ipc_client_connection.h"
#include "client/ipc_client_lift.h"
#include "client/ipc_client_service_link.h"

#include "ipc_client_generated.h"

#ifdef XRT_OS_WINDOWS
#include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//! A failed connect is retried no more often than this.
#define SERVICE_LINK_RETRY_NS (5ull * 1000 * 1000 * 1000)
//! ADR-061: the share poll makes its IPC round trip at most this often.
#define SHARE_POLL_INTERVAL_NS (250ull * 1000 * 1000)

struct ipc_client_service_link
{
	struct os_mutex mtx;
	enum ipc_client_service_link_state state;

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

	//! enum xrt_share_link_caps, fixed at create.
	uint32_t caps;

	/*
	 * ADR-061 share capability (SHARE links only). The registration is
	 * versioned: @ref reg_seq bumps on every change, @ref reg_sent_seq is what
	 * the CURRENT connection last acknowledged (reset per connection). Only
	 * the SHARE WORKER sends it (and polls): no API thread, no compositor
	 * thread ever waits on the service for registration or the poll.
	 */
	struct xrt_share_link_registration reg;
	uint64_t reg_seq;
	uint64_t reg_sent_seq;
	uint32_t reg_sent_gen; //!< the connection generation reg_sent_seq belongs to

	//! The service's answers (worker polls + frame_ready replies), single-report STOPPED.
	struct u_share_link_poll_cache poll;
	uint64_t next_poll_ns;

	//! The share worker (SHARE links): connects / retries, flushes the registration, polls.
	struct os_thread worker;
	struct os_cond worker_cond;
	bool worker_started;
	bool worker_stop;
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
	struct ipc_client_service_link *link = (struct ipc_client_service_link *)ptr;

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
			// Dial quietly: with no service every retry would otherwise log
			// the connect helpers' two ERRORs. connect_run's one WARN says it
			// instead; a made connection logs its errors as usual.
			xret = ipc_client_connection_init(c, U_LOGGING_RAW, &ii);
			if (xret == XRT_SUCCESS) {
				c->imc.log_level = U_LOGGING_ERROR;
			}
		}
	}

	if (xret == XRT_SUCCESS && (link->caps & XRT_SHARE_LINK_CAP_LIFT) != 0) {
		// ADR-053 (#2005): a LIFT link exists only for an instance that
		// enabled XR_DXR_lift, so every new connection carries the prewarm
		// hint (PrewarmOn = interest). Before publishing: nobody else holds c.
		(void)ipc_client_conv_hint_interest(c, 0);
	}

	// ADR-061: a SHARE link registers before it is published (nobody else
	// holds c yet). A registration that changes meanwhile is flushed by the
	// next kick / set / poll (reg_seq moved past what was sent here).
	uint64_t sent_seq = 0;
	bool registered = false;
	if (xret == XRT_SUCCESS && (link->caps & XRT_SHARE_LINK_CAP_SHARE) != 0) {
		os_mutex_lock(&link->mtx);
		struct xrt_share_link_registration reg = link->reg;
		sent_seq = link->reg_seq;
		os_mutex_unlock(&link->mtx);
		xrt_result_t rret = ipc_call_service_link_register(c, &reg);
		registered = rret == XRT_SUCCESS;
		if (!registered) {
			// An older service without the message refuses it: lift still works,
			// the session is simply not offered for sharing.
			U_LOG_W(
			    "[service-link] in-process: the service refused the share registration (%d); this "
			    "session is not offered for sharing",
			    (int)rret);
		}
	}

	os_mutex_lock(&link->mtx);
	if (xret == XRT_SUCCESS) {
		link->conn = c;
		link->gen++;
		link->state = IPC_CLIENT_SERVICE_LINK_CONNECTED;
		link->warned_unavailable = false;
		// What this connection has acknowledged; a refused registration is
		// not retried on this connection (the service will not take it).
		link->reg_sent_seq = registered ? sent_seq : link->reg_seq;
		link->reg_sent_gen = link->gen;
		u_share_link_poll_cache_reset(&link->poll);
		link->next_poll_ns = 0;
		U_LOG_I("[service-link] in-process: service connection up (generation %u, caps 0x%x)", link->gen,
		        link->caps);
		if (link->worker_started) {
			os_cond_signal(&link->worker_cond); // flush / poll on the new connection
		}
	} else {
		free(c); // init already finalised a half-made connection
		link->state = IPC_CLIENT_SERVICE_LINK_FAILED;
		link->retry_after_ns = os_monotonic_get_ns() + SERVICE_LINK_RETRY_NS;
		if (link->worker_started) {
			os_cond_signal(&link->worker_cond); // it schedules the retry
		}
		if (!link->warned_unavailable) {
			link->warned_unavailable = true;
			// Once until the next successful connect: the retries are quiet.
			U_LOG_W(
			    "[service-link] in-process: no service (%s); in-process conversion and sharing "
			    "unavailable, "
			    "retrying quietly every %u s",
			    c == NULL ? "service not running" : "connect refused or failed",
			    (unsigned)(SERVICE_LINK_RETRY_NS / 1000000000ull));
		}
	}
	link->connect_done = true;
	os_mutex_unlock(&link->mtx);
	return NULL;
}

//! Caller holds mtx. Joins a finished connect thread.
static void
join_finished_locked(struct ipc_client_service_link *link)
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
reap_dead_locked(struct ipc_client_service_link *link)
{
	if (link->state == IPC_CLIENT_SERVICE_LINK_DEAD && link->users == 0 && link->conn != NULL) {
		ipc_client_connection_fini(link->conn);
		free(link->conn);
		link->conn = NULL;
	}
}

//! Caller holds mtx. Starts a connect when none is in flight and one is due.
static void
maybe_start_connect_locked(struct ipc_client_service_link *link)
{
	join_finished_locked(link);
	reap_dead_locked(link);

	bool due = false;
	switch (link->state) {
	case IPC_CLIENT_SERVICE_LINK_CONNECTING: due = !link->thread_needs_join; break; // initial state
	case IPC_CLIENT_SERVICE_LINK_FAILED: due = os_monotonic_get_ns() >= link->retry_after_ns; break;
	case IPC_CLIENT_SERVICE_LINK_DEAD: due = link->conn == NULL; break; // old one closed
	case IPC_CLIENT_SERVICE_LINK_CONNECTED: due = false; break;
	}
	if (!due || link->thread_needs_join) {
		return;
	}

	link->state = IPC_CLIENT_SERVICE_LINK_CONNECTING;
	link->connect_done = false;
	if (os_thread_init(&link->thread) != 0 || os_thread_start(&link->thread, connect_run, link) != 0) {
		link->state = IPC_CLIENT_SERVICE_LINK_FAILED;
		link->retry_after_ns = os_monotonic_get_ns() + SERVICE_LINK_RETRY_NS;
		U_LOG_E("[service-link] in-process: could not start the service connect thread");
		return;
	}
	link->thread_needs_join = true;
}

static void *
share_worker_run(void *ptr);

xrt_result_t
ipc_client_service_link_create(const char *label, uint32_t caps, struct ipc_client_service_link **out_link)
{
	struct ipc_client_service_link *link = U_TYPED_CALLOC(struct ipc_client_service_link);
	if (link == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	if (os_mutex_init(&link->mtx) != 0) {
		free(link);
		return XRT_ERROR_ALLOCATION;
	}
	link->state = IPC_CLIENT_SERVICE_LINK_CONNECTING; // nothing in flight yet: first acquire starts it
	snprintf(link->label, sizeof(link->label), "%s", label != NULL ? label : "in-process");
	link->caps = caps;
	link->reg.caps = caps;
	if ((caps & XRT_SHARE_LINK_CAP_SHARE) != 0) {
		if (os_cond_init(&link->worker_cond) != 0) {
			os_mutex_destroy(&link->mtx);
			free(link);
			return XRT_ERROR_ALLOCATION;
		}
		if (os_thread_init(&link->worker) == 0 && os_thread_start(&link->worker, share_worker_run, link) == 0) {
			link->worker_started = true;
		} else {
			U_LOG_E(
			    "[service-link] in-process: could not start the share worker; this session is not "
			    "offered for sharing");
		}
	}
	*out_link = link;
	return XRT_SUCCESS;
}

void
ipc_client_service_link_destroy(struct ipc_client_service_link **link_ptr)
{
	if (link_ptr == NULL || *link_ptr == NULL) {
		return;
	}
	struct ipc_client_service_link *link = *link_ptr;
	*link_ptr = NULL;

	// The share worker first: it may hold the connection for one call.
	if (link->worker_started) {
		os_mutex_lock(&link->mtx);
		link->worker_stop = true;
		os_cond_signal(&link->worker_cond);
		os_mutex_unlock(&link->mtx);
		os_thread_join(&link->worker);
		os_thread_destroy(&link->worker);
		link->worker_started = false;
	}
	if ((link->caps & XRT_SHARE_LINK_CAP_SHARE) != 0) {
		os_cond_destroy(&link->worker_cond);
	}

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
ipc_client_service_link_acquire(struct ipc_client_service_link *link,
                                enum ipc_client_service_link_state *out_state,
                                uint32_t *out_gen)
{
	return ipc_client_service_link_acquire_wait(link, 0, out_state, out_gen);
}

struct ipc_connection *
ipc_client_service_link_acquire_wait(struct ipc_client_service_link *link,
                                     uint32_t timeout_ms,
                                     enum ipc_client_service_link_state *out_state,
                                     uint32_t *out_gen)
{
	if (out_gen != NULL) {
		*out_gen = 0;
	}
	if (link == NULL) {
		if (out_state != NULL) {
			*out_state = IPC_CLIENT_SERVICE_LINK_FAILED;
		}
		return NULL;
	}

	const uint64_t deadline = os_monotonic_get_ns() + (uint64_t)timeout_ms * 1000000ull;
	os_mutex_lock(&link->mtx);
	maybe_start_connect_locked(link);
	while (link->state == IPC_CLIENT_SERVICE_LINK_CONNECTING && timeout_ms > 0 &&
	       os_monotonic_get_ns() < deadline) {
		os_mutex_unlock(&link->mtx);
		os_nanosleep(10 * 1000 * 1000);
		os_mutex_lock(&link->mtx);
	}

	struct ipc_connection *c = NULL;
	if (link->state == IPC_CLIENT_SERVICE_LINK_CONNECTED && link->conn != NULL) {
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
ipc_client_service_link_release(struct ipc_client_service_link *link, uint32_t gen, bool ipc_failed)
{
	if (link == NULL) {
		return;
	}
	os_mutex_lock(&link->mtx);
	if (link->users > 0) {
		link->users--;
	}
	if (ipc_failed && gen == link->gen && link->state == IPC_CLIENT_SERVICE_LINK_CONNECTED) {
		link->state = IPC_CLIENT_SERVICE_LINK_DEAD;
		U_LOG_W(
		    "[service-link] in-process: the service connection (generation %u) is gone — its lift streams and "
		    "any share of it are lost; the session is unaffected and the link reconnects on the next use",
		    gen);
	}
	reap_dead_locked(link);
	os_mutex_unlock(&link->mtx);
}


uint32_t
ipc_client_service_link_caps(const struct ipc_client_service_link *link)
{
	return link != NULL ? link->caps : 0;
}


/*
 *
 * ADR-061: the share capability. Every call an app / compositor thread makes
 * here only takes the mutex; the share worker does the waiting on the
 * service (connect + 5 s retry, registration, the 250 ms poll). Only
 * frame_ready (one per published frame while a stream runs) and the ring
 * publish run on the caller's thread, because they carry its frame.
 *
 */

static bool
link_shares(const struct ipc_client_service_link *link)
{
	return link != NULL && (link->caps & XRT_SHARE_LINK_CAP_SHARE) != 0;
}

/*!
 * Caller holds mtx. Does the session want the service to know about it? Only
 * while it runs AND its compositor spoke (a tap, or a reason it has none):
 * until then nothing would be listed that the user could act on, so no
 * connection (no APP slot) is spent on it (review #2533 item 5).
 */
static bool
share_wants_connection_locked(const struct ipc_client_service_link *link)
{
	return link->reg.session_running != 0 &&
	       (link->reg.tap_available != 0 || link->reg.tap_unavailable_reason != 0);
}

static bool
reg_dirty_locked(const struct ipc_client_service_link *link)
{
	return link->reg_sent_gen != link->gen || link->reg_sent_seq != link->reg_seq;
}

static void *
share_worker_run(void *ptr)
{
	struct ipc_client_service_link *link = (struct ipc_client_service_link *)ptr;
	os_mutex_lock(&link->mtx);
	while (!link->worker_stop) {
		const uint64_t now = os_monotonic_get_ns();
		const bool want = share_wants_connection_locked(link);
		if (want && link->state != IPC_CLIENT_SERVICE_LINK_CONNECTED) {
			// The documented retry: a FAILED link re-dials every 5 s, a DEAD one
			// once its last user let go (maybe_start_connect_locked's rules).
			maybe_start_connect_locked(link);
		} else {
			join_finished_locked(link);
			reap_dead_locked(link);
		}

		const bool connected = link->state == IPC_CLIENT_SERVICE_LINK_CONNECTED && link->conn != NULL;
		if (connected && reg_dirty_locked(link)) {
			struct xrt_share_link_registration reg = link->reg;
			const uint64_t seq = link->reg_seq;
			const uint32_t gen = link->gen;
			struct ipc_connection *c = link->conn;
			link->users++;
			os_mutex_unlock(&link->mtx);
			xrt_result_t xret = ipc_call_service_link_register(c, &reg);
			ipc_client_service_link_release(link, gen, xret == XRT_ERROR_IPC_FAILURE);
			os_mutex_lock(&link->mtx);
			if (gen == link->gen) {
				link->reg_sent_gen = gen;
				link->reg_sent_seq = seq; // a refusal is not retried on this connection
				if (xret != XRT_SUCCESS && xret != XRT_ERROR_IPC_FAILURE) {
					U_LOG_W(
					    "[service-link] in-process: the service refused the share registration "
					    "(%d)",
					    (int)xret);
				}
			}
			continue;
		}
		if (connected && link->reg.session_running != 0 && now >= link->next_poll_ns) {
			const uint32_t gen = link->gen;
			struct ipc_connection *c = link->conn;
			link->users++;
			link->next_poll_ns = now + SHARE_POLL_INTERVAL_NS;
			os_mutex_unlock(&link->mtx);
			struct xrt_share_link_poll p;
			memset(&p, 0, sizeof(p));
			xrt_result_t xret = ipc_call_service_link_share_poll(c, &p);
			ipc_client_service_link_release(link, gen, xret == XRT_ERROR_IPC_FAILURE);
			os_mutex_lock(&link->mtx);
			if (xret == XRT_SUCCESS && gen == link->gen) {
				u_share_link_poll_cache_merge(&link->poll, &p);
			}
			continue;
		}

		// Sleep until the next poll / retry is due, or something changed.
		uint64_t wait_ns = SHARE_POLL_INTERVAL_NS;
		if (want && link->state == IPC_CLIENT_SERVICE_LINK_FAILED && link->retry_after_ns > now) {
			wait_ns = link->retry_after_ns - now;
		} else if (want && link->state == IPC_CLIENT_SERVICE_LINK_CONNECTING) {
			wait_ns = 50ull * 1000 * 1000; // the connect thread finishes on its own; look again soon
		} else if (connected && link->next_poll_ns > now && link->reg.session_running != 0) {
			wait_ns = link->next_poll_ns - now;
		}
		os_cond_wait_timeout_ns(&link->worker_cond, &link->mtx, wait_ns);
	}
	os_mutex_unlock(&link->mtx);
	return NULL;
}

//! Caller holds mtx.
static void
worker_wake_locked(struct ipc_client_service_link *link)
{
	if (link->worker_started) {
		os_cond_signal(&link->worker_cond);
	}
}

void
ipc_client_service_link_kick(struct ipc_client_service_link *link)
{
	if (!link_shares(link)) {
		return;
	}
	os_mutex_lock(&link->mtx);
	worker_wake_locked(link);
	os_mutex_unlock(&link->mtx);
}

void
ipc_client_service_link_set_registration(struct ipc_client_service_link *link,
                                         const struct xrt_share_link_registration *reg)
{
	if (!link_shares(link) || reg == NULL) {
		return;
	}
	os_mutex_lock(&link->mtx);
	// The tap fields and the adapter belong to the compositor (set_tap): an
	// update from the state tracker never overwrites them.
	const struct xrt_share_link_registration keep = link->reg;
	link->reg = *reg;
	link->reg.caps = link->caps;
	link->reg.tap_available = keep.tap_available;
	link->reg.tap_unavailable_reason = keep.tap_unavailable_reason;
	link->reg.luid_valid = keep.luid_valid;
	memcpy(link->reg.luid, keep.luid, sizeof(link->reg.luid));
	link->reg_seq++;
	worker_wake_locked(link); // the worker sends it; this thread never waits on the service
	os_mutex_unlock(&link->mtx);
}

void
ipc_client_service_link_set_tap(struct ipc_client_service_link *link,
                                bool available,
                                uint32_t unavailable_reason,
                                const uint8_t luid[XRT_LUID_SIZE],
                                bool luid_valid)
{
	if (!link_shares(link)) {
		return;
	}
	os_mutex_lock(&link->mtx);
	struct xrt_share_link_registration *r = &link->reg;
	const uint32_t a = available ? 1u : 0u;
	const uint32_t why = available ? 0u : unavailable_reason;
	bool changed = r->tap_available != a || r->tap_unavailable_reason != why;
	if (luid_valid && luid != NULL) {
		changed |= r->luid_valid == 0 || memcmp(r->luid, luid, XRT_LUID_SIZE) != 0;
		memcpy(r->luid, luid, XRT_LUID_SIZE);
		r->luid_valid = 1;
	} else {
		changed |= r->luid_valid != 0; // a stale adapter is cleared, not kept
		memset(r->luid, 0, sizeof(r->luid));
		r->luid_valid = 0;
	}
	r->tap_available = a;
	r->tap_unavailable_reason = why;
	if (changed) {
		link->reg_seq++;
		worker_wake_locked(link);
	}
	os_mutex_unlock(&link->mtx);
}

bool
ipc_client_service_link_share_poll(struct ipc_client_service_link *link, struct xrt_share_link_poll *out)
{
	if (out != NULL) {
		memset(out, 0, sizeof(*out));
	}
	if (!link_shares(link) || out == NULL) {
		return false;
	}
	// Never an IPC call: the worker polls; this reads its newest answer.
	os_mutex_lock(&link->mtx);
	bool ok = false;
	if (link->state == IPC_CLIENT_SERVICE_LINK_CONNECTED && link->conn != NULL) {
		ok = u_share_link_poll_cache_take(&link->poll, out);
	}
	os_mutex_unlock(&link->mtx);
	return ok;
}

//! Acquire for a share call: CONNECTED only, never starts a connect from a per-frame path.
static struct ipc_connection *
share_acquire(struct ipc_client_service_link *link, uint32_t *out_gen)
{
	struct ipc_connection *c = NULL;
	os_mutex_lock(&link->mtx);
	if (link->state == IPC_CLIENT_SERVICE_LINK_CONNECTED && link->conn != NULL) {
		c = link->conn;
		link->users++;
		*out_gen = link->gen;
	}
	os_mutex_unlock(&link->mtx);
	return c;
}

xrt_result_t
ipc_client_service_link_share_publish_ring(struct ipc_client_service_link *link,
                                           const struct xrt_share_link_ring_desc *desc,
                                           const xrt_graphics_buffer_handle_t *textures,
                                           uint32_t texture_count,
                                           xrt_graphics_sync_handle_t fence)
{
	if (!link_shares(link) || desc == NULL || (texture_count != 0 && textures == NULL) ||
	    texture_count > XRT_SHARE_LINK_RING_SLOTS) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	uint32_t gen = 0;
	struct ipc_connection *c = share_acquire(link, &gen);
	if (c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct xrt_share_link_ring_desc d = *desc;
	d.has_fence = xrt_graphics_sync_handle_is_valid(fence) ? 1u : 0u;
	xrt_result_t xret = ipc_call_service_link_share_publish_ring(c, &d, textures, texture_count);
	if (xret == XRT_SUCCESS && d.has_fence != 0) {
		// A failure here leaves the ring waiting for a fence that never comes:
		// the service time-boxes that and ends the share (SHARE_STOPPED).
		xret = ipc_call_service_link_share_publish_fence(c, d.generation, &fence, 1);
	}
	// Only a dead pipe kills the link; a refusal is just reported.
	ipc_client_service_link_release(link, gen, xret == XRT_ERROR_IPC_FAILURE);
	return xret;
}

xrt_result_t
ipc_client_service_link_share_frame_ready(struct ipc_client_service_link *link,
                                          const struct xrt_share_link_frame *frame,
                                          struct xrt_share_link_poll *out_poll)
{
	if (out_poll != NULL) {
		memset(out_poll, 0, sizeof(*out_poll));
	}
	if (!link_shares(link) || frame == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	uint32_t gen = 0;
	struct ipc_connection *c = share_acquire(link, &gen);
	if (c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct xrt_share_link_poll p;
	memset(&p, 0, sizeof(p));
	xrt_result_t xret = ipc_call_service_link_share_frame_ready(c, frame, &p);
	ipc_client_service_link_release(link, gen, xret == XRT_ERROR_IPC_FAILURE);
	os_mutex_lock(&link->mtx);
	if (xret == XRT_SUCCESS && gen == link->gen) {
		u_share_link_poll_cache_merge(&link->poll, &p);
		link->next_poll_ns = os_monotonic_get_ns() + SHARE_POLL_INTERVAL_NS;
	}
	if (out_poll != NULL) {
		// The same single-report rule as the poll: a STOPPED is taken here once.
		(void)u_share_link_poll_cache_take(&link->poll, out_poll);
	}
	os_mutex_unlock(&link->mtx);
	return xret;
}
