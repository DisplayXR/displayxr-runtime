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

// Forward decl from state_trackers/oxr/oxr_mcp_tools.h — we intentionally
// don't pull in that header (which includes openxr.h) from aux_util.
// The signature must stay in sync with oxr_mcp_capture_fn there.
typedef bool (*oxr_mcp_capture_fn)(const char *path, void *userdata);
extern void
oxr_mcp_tools_set_capture_handler(oxr_mcp_capture_fn fn, void *userdata);

#define CAPTURE_TIMEOUT_MS 500 //!< Give the compositor ~30 frames at 60 Hz.

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

// Invoked on the MCP server thread. Hands off to the compositor thread
// and blocks until done or timeout.
static bool
handler_adapter(const char *path_prefix, void *userdata)
{
	struct u_mcp_capture_request *req = userdata;
	if (req == NULL || path_prefix == NULL) {
		return false;
	}

	pthread_mutex_lock(&req->lock);
	// Truncate safely — path_prefix is caller-bounded but be defensive.
	size_t n = strlen(path_prefix);
	if (n >= sizeof(req->path_prefix)) {
		n = sizeof(req->path_prefix) - 1;
	}
	memcpy(req->path_prefix, path_prefix, n);
	req->path_prefix[n] = '\0';
	// Default to full atlas + L/R until the tool surfaces a view arg;
	// once oxr_mcp_tools threads a view through userdata we'll honor it.
	req->views = U_MCP_CAPTURE_ALL;
	req->pending = true;
	req->done = false;
	req->views_written = 0;

	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += CAPTURE_TIMEOUT_MS / 1000;
	deadline.tv_nsec += (CAPTURE_TIMEOUT_MS % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec += 1;
		deadline.tv_nsec -= 1000000000L;
	}

	while (!req->done) {
		int rc = pthread_cond_timedwait(&req->cond, &req->lock, &deadline);
		if (rc != 0) {
			// Timed out — abandon; any late completion from the
			// compositor will find !pending and no-op. We leak
			// nothing because the buffer is on the struct.
			req->pending = false;
			pthread_mutex_unlock(&req->lock);
			return false;
		}
	}

	bool ok = req->views_written != 0;
	req->pending = false;
	pthread_mutex_unlock(&req->lock);
	return ok;
}

void
u_mcp_capture_install(struct u_mcp_capture_request *req)
{
	oxr_mcp_tools_set_capture_handler(handler_adapter, req);
}

void
u_mcp_capture_uninstall(void)
{
	oxr_mcp_tools_set_capture_handler(NULL, NULL);
}

bool
u_mcp_capture_poll(struct u_mcp_capture_request *req, char *out_path_prefix, uint32_t *out_views)
{
	pthread_mutex_lock(&req->lock);
	bool pending = req->pending && !req->done;
	if (pending) {
		memcpy(out_path_prefix, req->path_prefix, sizeof(req->path_prefix));
		*out_views = req->views;
	}
	pthread_mutex_unlock(&req->lock);
	return pending;
}

void
u_mcp_capture_complete(struct u_mcp_capture_request *req, uint32_t views_written)
{
	pthread_mutex_lock(&req->lock);
	req->views_written = views_written;
	req->done = true;
	pthread_cond_signal(&req->cond);
	pthread_mutex_unlock(&req->lock);
}
