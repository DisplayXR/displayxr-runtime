// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-Linux service-surface input → workspace input routing (#710).
 * @ingroup ipc_server
 *
 * The Linux twin of the input half of ipc_server_macos_appkit.m: installs the
 * comp_window_linux input sink, translates each decoded surface event into an
 * @ref ipc_workspace_input_event, publishes the pointer position for the
 * workspace cursor composite, and routes it with @ref ipc_server_input_route
 * to the controller's queue and/or the content client under the cursor /
 * with focus. Clients drain through ipc_handle_workspace_enumerate_input_events.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct ipc_server;

//! Install the surface input sink (server init). Safe before the window exists.
void
ipc_server_linux_input_install(struct ipc_server *s);

//! Remove the sink (server teardown).
void
ipc_server_linux_input_uninstall(void);

#ifdef __cplusplus
}
#endif
