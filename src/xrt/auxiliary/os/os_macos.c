// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS-specific shared state (window-closed flag).
 * @author David Fattal
 * @ingroup aux_os
 *
 * This file provides the macOS window-closed atomic flag and its accessors.
 * It lives in aux_os so all targets (libopenxr_monado, monado-service,
 * monado-cli) can link it — st_oxr writes it, comp_multi reads it.
 */

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_MACOS

#include <stdatomic.h>
#include <stdbool.h>

static atomic_bool g_macos_window_closed = false;

bool
oxr_macos_window_closed(void)
{
	return atomic_load(&g_macos_window_closed);
}

void
oxr_macos_reset_window_closed(void)
{
	atomic_store(&g_macos_window_closed, false);
}

void
oxr_macos_set_window_closed(void)
{
	atomic_store(&g_macos_window_closed, true);
}

#endif // XRT_OS_MACOS
