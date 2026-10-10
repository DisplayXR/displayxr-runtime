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

#include <string.h>

static void
surface_input_sink(void *userdata, const struct comp_window_linux_input *in)
{
	(void)userdata;

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
		comp_multi_workspace_set_pointer_px(in->x, in->y);
		ev.event_type = IPC_WORKSPACE_INPUT_EVENT_POINTER;
		ev.u.pointer.button = in->button;
		ev.u.pointer.is_down = in->is_down ? 1u : 0u;
		ev.u.pointer.cursor_x = in->x;
		ev.u.pointer.cursor_y = in->y;
		ev.u.pointer.modifiers = in->modifiers;
		break;
	case COMP_WINDOW_LINUX_INPUT_MOTION:
		// The cursor sprite follows this position; the controller hit-tests
		// hover from the routed POINTER_MOTION.
		comp_multi_workspace_set_pointer_px(in->x, in->y);
		ev.event_type = IPC_WORKSPACE_INPUT_EVENT_POINTER_MOTION;
		ev.u.pointer_motion.cursor_x = in->x;
		ev.u.pointer_motion.cursor_y = in->y;
		ev.u.pointer_motion.button_mask = in->button_mask;
		ev.u.pointer_motion.modifiers = in->modifiers;
		break;
	case COMP_WINDOW_LINUX_INPUT_SCROLL:
		ev.event_type = IPC_WORKSPACE_INPUT_EVENT_SCROLL;
		ev.u.scroll.delta_y = in->scroll_delta;
		ev.u.scroll.cursor_x = in->x;
		ev.u.scroll.cursor_y = in->y;
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
	(void)s;
	comp_window_linux_set_input_sink(surface_input_sink, NULL);
	U_LOG_I("Linux service surface input routed to the workspace input queues");
}

void
ipc_server_linux_input_uninstall(void)
{
	comp_window_linux_set_input_sink(NULL, NULL);
}
