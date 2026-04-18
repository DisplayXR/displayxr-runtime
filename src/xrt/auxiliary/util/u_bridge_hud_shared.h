// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shared memory struct for WebXR bridge HUD overlay.
 *
 * The bridge creates a named file mapping ("DisplayXR_BridgeHUD") and writes
 * HUD text lines from the WebXR sample. The compositor opens the same mapping
 * and reads the lines to render on the back buffer via u_hud.
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_HUD_MAGIC      0x48554442  // "HUDB"
#define BRIDGE_HUD_VERSION    1
#define BRIDGE_HUD_MAX_LINES  8
#define BRIDGE_HUD_LABEL_LEN  16
#define BRIDGE_HUD_TEXT_LEN   128
#define BRIDGE_HUD_MAPPING_NAME L"DisplayXR_BridgeHUD"

struct bridge_hud_line
{
	char label[BRIDGE_HUD_LABEL_LEN]; //!< Section label (e.g. "Mode", "Eyes")
	char text[BRIDGE_HUD_TEXT_LEN];   //!< Section content
};

/*!
 * Shared memory layout for bridge HUD overlay.
 * Written by the bridge process, read by the compositor (service) process.
 */
struct bridge_hud_shared
{
	uint32_t magic;      //!< Must be BRIDGE_HUD_MAGIC
	uint32_t version;    //!< Must be BRIDGE_HUD_VERSION
	uint32_t visible;    //!< Non-zero if HUD should be displayed
	uint32_t line_count; //!< Number of valid lines (0..BRIDGE_HUD_MAX_LINES)
	struct bridge_hud_line lines[BRIDGE_HUD_MAX_LINES];
};

#ifdef __cplusplus
}
#endif
