// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_mcp_tools — turn an app's submitted result into the MCP
 *         tool result handed to the displayxr-mcp framework.
 * @ingroup oxr_main
 *
 * Pure (cJSON only, no session, no lock) so the rule is host-testable
 * (tests/tests_oxr_mcp_app_result.cpp).
 */

#pragma once

#include <cjson/cJSON.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Build the tool result for an answered call. Never returns NULL (NULL is the
 * framework's "tool handler failed" JSON-RPC error, which drops the message).
 *
 * - @p success: @p result_json parsed as-is; NULL → `{}`; unparseable JSON →
 *   `{"raw":"<text>"}`.
 * - !@p success: a tool error the agent can read. An object keeps every member
 *   and gains `"isError":true` as its first member (an `{}` also gains
 *   `"error":"tool failed"`); any other value becomes
 *   `{"isError":true,"error":<value>}`, unparseable text
 *   `{"isError":true,"error":"<text>"}`. displayxr-mcp >= v0.5.6 lifts the
 *   flag to `result.isError` (MCP CallToolResult.isError); older frameworks
 *   still deliver the message in the result text.
 */
cJSON *
oxr_mcp_app_tool_result(bool success, const char *result_json);

#ifdef __cplusplus
}
#endif
