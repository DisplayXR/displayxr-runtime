// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2024-2025, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface for IPC server code.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup ipc_server
 */

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_config_os.h"

#ifndef XRT_OS_ANDROID
#include "util/u_debug_gui.h"
#endif


#ifdef __cplusplus
extern "C" {
#endif


#ifndef XRT_OS_ANDROID

/*!
 * Information passed into the IPC server main function, used for customization
 * of the IPC server.
 *
 * @ingroup ipc_server
 */
struct ipc_server_main_info
{
	//! Information passed onto the debug gui.
	struct u_debug_gui_create_info udgci;

	//! When true, service runs in workspace mode with a shared multi-compositor window.
	bool workspace_mode;

	//! ADR-045: when true, the server ends its main loop (with
	//! @ref ipc_server_restart_requested set) once a refresh has adopted a
	//! better display plug-in under a head device the previous one created
	//! and no client is connected, so the host can start a fresh instance
	//! built on the new plug-in end to end. Only the standalone service sets
	//! it; it must also know how to start that successor.
	bool allow_adoption_restart;
};

/*!
 * ADR-045 R-c: ask the server to re-evaluate display-processor selection
 * (`refresh_display_processors`) because the world changed — display
 * topology, device nodes, or the plug-in registration root. Cheap and
 * non-blocking: it only flags a request for the server's re-probe worker,
 * which debounces bursts (>= 1 s) and runs the refresh off the caller's
 * thread, so it is safe from a window procedure or a registry waiter. A no-op
 * before the server has started or after it stopped. @p reason must be a
 * string literal (logged, not copied).
 *
 * @ingroup ipc_server
 */
void
ipc_server_request_display_reprobe(const char *reason);

/*!
 * True after @ref ipc_server_main returned because of an adoption restart
 * (see @ref ipc_server_main_info::allow_adoption_restart): the host should
 * start a successor instance.
 *
 * @ingroup ipc_server
 */
bool
ipc_server_restart_requested(void);

/*!
 * Main entrypoint to the compositor process.
 *
 * @ingroup ipc_server
 */
int
ipc_server_main(int argc, char **argv, const struct ipc_server_main_info *ismi);

#endif


#ifdef XRT_OS_ANDROID

/*!
 * Main entrypoint to the server process.
 *
 * @param ps Pointer to populate with the server struct.
 * @param startup_complete_callback Function to call upon completing startup
 *                                  and populating *ps, but before entering
 *                                  the mainloop.
 * @param data user data to pass to your callback.
 *
 * @ingroup ipc_server
 */
int
ipc_server_main_android(struct ipc_server **ps, void (*startup_complete_callback)(void *data), void *data);

#endif


#ifdef __cplusplus
}
#endif
