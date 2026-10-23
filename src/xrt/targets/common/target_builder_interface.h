// Copyright 2022-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  List of all @ref xrt_builder creation functions.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 */

#include "xrt/xrt_config_build.h"
#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_config_have.h"


/*
 *
 * Config checking, sorted alphabetically.
 *
 */

// Vendor builders (Leia SR, etc.) are no longer compiled into the
// runtime DLL. The Leia plug-in (DisplayXR-LeiaSR.dll, shipped from
// DisplayXR/displayxr-leia-plugin) is loaded by target_plugin_loader.c
// at xrCreateInstance time and resolved through the sim_display
// builder's iface-aware fallback. See ADR-019 / issues #256 & #263.

// Always enabled as fallback — use FORCE_SIM_DISPLAY=1 to override vendor drivers
#define T_BUILDER_SIM_DISPLAY


/*
 *
 * Setter upper creation functions, sorted alphabetically.
 *
 */

#ifdef T_BUILDER_SIM_DISPLAY
/*!
 * Builder for simulated 3D display (SBS/anaglyph/blend output).
 */
struct xrt_builder *
t_builder_sim_display_create(void);
#endif
