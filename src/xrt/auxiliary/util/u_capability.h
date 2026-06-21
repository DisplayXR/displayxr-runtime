// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime capability gate: machine marker (HKLM Capabilities) + env override.
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Resolve a tri-state runtime-capability flag with the precedence:
 *
 *   1. environment variable @p env_var, if set and non-empty — the dev override
 *      ("0" / "false" / "off" / "no" → false, anything else → true);
 *   2. else the machine capability marker, if present:
 *      `HKLM\Software\DisplayXR\Capabilities\<cap_name>\Enabled` (REG_DWORD) on
 *      Windows, or `/Library/Application Support/DisplayXR/Capabilities/<cap_name>/Enabled`
 *      (first byte '1') on macOS/Linux;
 *   3. else @p default_value.
 *
 * This mirrors the MCP capability convention
 * (`oxr_mcp_capability_enabled()` + `mcp_check_env_or()`) but is parameterized by
 * name and lets the caller choose the default — so a feature can default ON while
 * still honouring an admin force-OFF marker and a dev env override.
 *
 * @param env_var       Environment variable name (may be NULL to skip the override).
 * @param cap_name      Capability marker name under the Capabilities key (may be NULL).
 * @param default_value Result when neither the env var nor the marker is present.
 *
 * @ingroup aux_util
 */
bool
u_capability_enabled(const char *env_var, const char *cap_name, bool default_value);

#ifdef __cplusplus
}
#endif
