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

	//! When true, service runs in shell mode with a shared multi-compositor window.
	bool shell_mode;
};

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
