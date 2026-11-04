// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-061 arm 2: an in-process session's service link at session
 *         begin / end — the share capability's half of the link
 *         (ADR-049 Amendment 1). The lift half is oxr_conv.c.
 * @ingroup oxr_main
 *
 * While sharing is on (DXR_APP_SHARE != 0 in the app's environment, the
 * default) and the session is one a tap can serve (Windows D3D11 / D3D12 /
 * Vulkan in-process), xrBeginSession hands the link its "running"
 * registration and installs it as the process's share sink
 * (util/u_share_link.h) so its compositor's share tap reaches it. Nothing here
 * waits on the service: the link's worker dials (once the compositor reported
 * its tap state), registers and polls. xrEndSession re-registers "not
 * running" (the service stops listing it) and removes the sink; the link
 * itself lives until the session is destroyed, because the session's lift
 * streams may still ride it.
 */

#include "oxr_objects.h"
#include "oxr_conv_gpu.h"
#include "oxr_service_link.h"

#include "client/ipc_client_service_link.h"
#include "util/u_logging.h"
#include "util/u_share_link.h"

#include <string.h>

#ifdef OXR_HAVE_DXR_lift

static void
sink_set_tap(void *ctx, bool available, uint32_t reason, const uint8_t luid[XRT_LUID_SIZE], bool luid_valid)
{
	ipc_client_service_link_set_tap((struct ipc_client_service_link *)ctx, available, reason, luid, luid_valid);
}

static bool
sink_poll(void *ctx, struct xrt_share_link_poll *out)
{
	return ipc_client_service_link_share_poll((struct ipc_client_service_link *)ctx, out);
}

static xrt_result_t
sink_publish_ring(void *ctx,
                  const struct xrt_share_link_ring_desc *desc,
                  const xrt_graphics_buffer_handle_t *textures,
                  uint32_t texture_count,
                  xrt_graphics_sync_handle_t fence)
{
	return ipc_client_service_link_share_publish_ring((struct ipc_client_service_link *)ctx, desc, textures,
	                                                  texture_count, fence);
}

static xrt_result_t
sink_frame_ready(void *ctx, const struct xrt_share_link_frame *frame, struct xrt_share_link_poll *out_poll)
{
	return ipc_client_service_link_share_frame_ready((struct ipc_client_service_link *)ctx, frame, out_poll);
}

static const struct u_share_link_sink k_sink = {
    .set_tap = sink_set_tap,
    .poll = sink_poll,
    .publish_ring = sink_publish_ring,
    .frame_ready = sink_frame_ready,
};

void
oxr_service_link_session_begin(struct oxr_session *sess)
{
	if (!oxr_service_link_share_wanted(sess)) {
		return;
	}
	struct ipc_client_service_link *link = oxr_conv_link_get(sess);
	if (link == NULL || (ipc_client_service_link_caps(link) & XRT_SHARE_LINK_CAP_SHARE) == 0) {
		return;
	}
	struct xrt_share_link_registration reg;
	oxr_service_link_fill_registration(sess, true, &reg);
	ipc_client_service_link_set_registration(link, &reg);
	if (!sess->share_sink_installed) {
		u_share_link_install(&k_sink, link);
		sess->share_sink_installed = true;
	}
	// The worker connects / registers in the background; this never blocks.
	ipc_client_service_link_kick(link);
	U_LOG_I("app share: in-process session offered for sharing over its service link%s",
	        sess->app_share_opt_out ? " (opted out: registered, never listed)" : "");
}

void
oxr_service_link_session_end(struct oxr_session *sess)
{
	if (sess == NULL || sess->service_link == NULL) {
		return;
	}
	if (sess->share_sink_installed) {
		u_share_link_uninstall(sess->service_link);
		sess->share_sink_installed = false;
	}
	if ((ipc_client_service_link_caps(sess->service_link) & XRT_SHARE_LINK_CAP_SHARE) != 0) {
		struct xrt_share_link_registration reg;
		oxr_service_link_fill_registration(sess, false, &reg);
		ipc_client_service_link_set_registration(sess->service_link, &reg);
	}
}

void
oxr_service_link_session_fini(struct oxr_session *sess)
{
	if (sess != NULL && sess->share_sink_installed && sess->service_link != NULL) {
		u_share_link_uninstall(sess->service_link);
		sess->share_sink_installed = false;
	}
}

#else // !OXR_HAVE_DXR_lift

void
oxr_service_link_session_begin(struct oxr_session *sess)
{
	(void)sess;
}

void
oxr_service_link_session_end(struct oxr_session *sess)
{
	(void)sess;
}

void
oxr_service_link_session_fini(struct oxr_session *sess)
{
	(void)sess;
}

#endif
