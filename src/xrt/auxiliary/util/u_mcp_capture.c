// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  MCP capture_frame cross-thread hand-off.
 * @ingroup aux_util
 */

#include "u_mcp_capture.h"

#include <string.h>
#include <time.h>

// Optional callback for notifying the Phase A per-PID capture handler
// (oxr_mcp_tools). Registered by oxr_instance_create at runtime; NULL
// in the service process which has no state tracker.
typedef void (*u_mcp_capture_notify_fn)(void *req);
static u_mcp_capture_notify_fn g_notify_install = NULL;
static u_mcp_capture_notify_fn g_notify_uninstall = NULL;

/*! PNG encode of a 3024×1964 atlas is ~80–150 ms on an M1; 3 s is
 *  ample headroom while still tripping on a genuine stall. */
#define CAPTURE_TIMEOUT_MS 3000

static struct u_mcp_capture_request *g_installed_req = NULL;

void
u_mcp_capture_init(struct u_mcp_capture_request *req)
{
	memset(req, 0, sizeof(*req));
	pthread_mutex_init(&req->lock, NULL);
	pthread_cond_init(&req->cond, NULL);
}

void
u_mcp_capture_fini(struct u_mcp_capture_request *req)
{
	pthread_cond_destroy(&req->cond);
	pthread_mutex_destroy(&req->lock);
}

bool
u_mcp_capture_blocking_handler(const char *path, void *userdata)
{
	struct u_mcp_capture_request *req = userdata;
	if (req == NULL || path == NULL) {
		return false;
	}

	pthread_mutex_lock(&req->lock);
	size_t n = strlen(path);
	if (n >= sizeof(req->path)) {
		n = sizeof(req->path) - 1;
	}
	memcpy(req->path, path, n);
	req->path[n] = '\0';
	req->pending = true;
	req->done = false;
	req->success = false;

	// Absolute deadline for pthread_cond_timedwait. C11's timespec_get is
	// portable (POSIX CLOCK_REALTIME / MSVC alike); clock_gettime isn't
	// — MSVC's <time.h> has no CLOCK_REALTIME symbol.
	struct timespec deadline;
	(void)timespec_get(&deadline, TIME_UTC);
	deadline.tv_sec += CAPTURE_TIMEOUT_MS / 1000;
	deadline.tv_nsec += (CAPTURE_TIMEOUT_MS % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec += 1;
		deadline.tv_nsec -= 1000000000L;
	}

	while (!req->done) {
		int rc = pthread_cond_timedwait(&req->cond, &req->lock, &deadline);
		if (rc != 0) {
			req->pending = false;
			pthread_mutex_unlock(&req->lock);
			return false;
		}
	}

	bool ok = req->success;
	req->pending = false;
	pthread_mutex_unlock(&req->lock);
	return ok;
}

void
u_mcp_capture_install(struct u_mcp_capture_request *req)
{
	g_installed_req = req;
	if (g_notify_install) {
		g_notify_install(req);
	}
}

void
u_mcp_capture_uninstall(void)
{
	if (g_notify_uninstall) {
		g_notify_uninstall(NULL);
	}
	g_installed_req = NULL;
}

void
u_mcp_capture_set_notify(u_mcp_capture_notify_fn on_install,
                         u_mcp_capture_notify_fn on_uninstall)
{
	g_notify_install = on_install;
	g_notify_uninstall = on_uninstall;
}

struct u_mcp_capture_request *
u_mcp_capture_get_installed(void)
{
	return g_installed_req;
}

bool
u_mcp_capture_poll(struct u_mcp_capture_request *req, char *out_path)
{
	pthread_mutex_lock(&req->lock);
	bool pending = req->pending && !req->done;
	if (pending) {
		memcpy(out_path, req->path, sizeof(req->path));
	}
	pthread_mutex_unlock(&req->lock);
	return pending;
}

void
u_mcp_capture_complete(struct u_mcp_capture_request *req, bool success)
{
	pthread_mutex_lock(&req->lock);
	req->success = success;
	req->done = true;
	pthread_cond_signal(&req->cond);
	pthread_mutex_unlock(&req->lock);
}
