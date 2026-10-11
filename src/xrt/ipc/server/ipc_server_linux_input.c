// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-Linux service-surface input → workspace input routing (#710).
 * @author David Fattal
 * @ingroup ipc_server
 */

#include "ipc_server_linux_input.h"
#include "ipc_server_input_queue.h"

#include "main/comp_window_linux.h"
#include "multi/comp_multi_workspace.h"
#include "shared/ipc_protocol.h"

#include "util/u_logging.h"
#include "server/ipc_server.h"
#include "xrt/xrt_compositor.h"

#include <string.h>

/*!
 * Surface px -> the workspace's display px. The shared surface lays its atlas
 * out in the panel's REPORTED pixels (xrt_system_compositor_info
 * display_pixel_*), and the controller hit-tests / places in that space too,
 * but the window's buffer is whatever the output really is — a sim-display
 * 1920x1080 panel on a 2880x1800 laptop output, say. Without this, pointer
 * events and the cursor sprite land 1.5x too far right / down there. Identity
 * when the sizes match (every real panel).
 */
static void
to_display_px(struct ipc_server *s, const struct comp_window_linux_input *in, int64_t *out_x, int64_t *out_y)
{
	*out_x = in->x;
	*out_y = in->y;
	if (s == NULL || s->xsysc == NULL || in->surface_w == 0 || in->surface_h == 0) {
		return;
	}
	const uint32_t dw = s->xsysc->info.display_pixel_width;
	const uint32_t dh = s->xsysc->info.display_pixel_height;
	if (dw == 0 || dh == 0 || (dw == in->surface_w && dh == in->surface_h)) {
		return;
	}
	*out_x = (int64_t)in->x * dw / in->surface_w;
	*out_y = (int64_t)in->y * dh / in->surface_h;
}

static void
surface_input_sink(void *userdata, const struct comp_window_linux_input *in)
{
	struct ipc_server *s = (struct ipc_server *)userdata;
	int64_t px = 0, py = 0;
	to_display_px(s, in, &px, &py);

	struct ipc_workspace_input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.timestamp_ms = in->timestamp_ms;

	switch (in->type) {
	case COMP_WINDOW_LINUX_INPUT_KEY:
		ev.event_type = IPC_WORKSPACE_INPUT_EVENT_KEY;
		ev.u.key.vk_code = in->vk_code;
		ev.u.key.is_down = in->is_down ? 1u : 0u;
		ev.u.key.modifiers = in->modifiers;
		break;
	case COMP_WINDOW_LINUX_INPUT_BUTTON:
		comp_multi_workspace_set_pointer_px((int32_t)px, (int32_t)py);
		ev.event_type = IPC_WORKSPACE_INPUT_EVENT_POINTER;
		ev.u.pointer.button = in->button;
		ev.u.pointer.is_down = in->is_down ? 1u : 0u;
		ev.u.pointer.cursor_x = px;
		ev.u.pointer.cursor_y = py;
		ev.u.pointer.modifiers = in->modifiers;
		break;
	case COMP_WINDOW_LINUX_INPUT_MOTION:
		// The cursor sprite follows this position; the controller hit-tests
		// hover from the routed POINTER_MOTION.
		comp_multi_workspace_set_pointer_px((int32_t)px, (int32_t)py);
		ev.event_type = IPC_WORKSPACE_INPUT_EVENT_POINTER_MOTION;
		ev.u.pointer_motion.cursor_x = px;
		ev.u.pointer_motion.cursor_y = py;
		ev.u.pointer_motion.button_mask = in->button_mask;
		ev.u.pointer_motion.modifiers = in->modifiers;
		break;
	case COMP_WINDOW_LINUX_INPUT_SCROLL:
		ev.event_type = IPC_WORKSPACE_INPUT_EVENT_SCROLL;
		ev.u.scroll.delta_y = in->scroll_delta;
		ev.u.scroll.cursor_x = px;
		ev.u.scroll.cursor_y = py;
		ev.u.scroll.modifiers = in->modifiers;
		break;
	case COMP_WINDOW_LINUX_INPUT_FOCUS_LOST:
		// No wire event: a client sees its keys stop. A key held across the
		// loss gets no release; the controller re-syncs on its next press.
		return;
	default: return;
	}

	ipc_server_input_route(&ev);
}

void
ipc_server_linux_input_install(struct ipc_server *s)
{
	comp_window_linux_set_input_sink(surface_input_sink, s);
	U_LOG_I("Linux service surface input routed to the workspace input queues");
}

void
ipc_server_linux_input_uninstall(void)
{
	comp_window_linux_set_input_sink(NULL, NULL);
}
