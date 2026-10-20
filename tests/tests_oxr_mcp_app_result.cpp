// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_mcp_tools: what an app's xrSubmitMCPToolResultDXR becomes for
 *         the MCP framework. An XR_FALSE submit must be a tool error that keeps
 *         the app's message ("isError":true), never NULL — NULL is the
 *         framework's bare JSON-RPC -32000 "tool handler failed".
 */

#include "catch_amalgamated.hpp"

#include "oxr_mcp_app_result.h"

#include <cstdlib>
#include <string>

namespace {

std::string
shape(bool success, const char *json)
{
	cJSON *out = oxr_mcp_app_tool_result(success, json);
	REQUIRE(out != nullptr);
	char *s = cJSON_PrintUnformatted(out);
	std::string r = s;
	cJSON_free(s);
	cJSON_Delete(out);
	return r;
}

} // namespace

TEST_CASE("mcp_app_result: success passes the app's JSON through", "[oxr][mcp]")
{
	CHECK(shape(true, "{\"ok\":true}") == "{\"ok\":true}");
	CHECK(shape(true, "[1,2]") == "[1,2]");
	CHECK(shape(true, nullptr) == "{}");
	CHECK(shape(true, "") == "{}");
	CHECK(shape(true, "not json") == "{\"raw\":\"not json\"}");
	// A success never gains the flag.
	CHECK(shape(true, "{\"said\":\"hi\"}").find("isError") == std::string::npos);
}

TEST_CASE("mcp_app_result: failure keeps the message as an isError result", "[oxr][mcp]")
{
	// What a throwing Unity handler submits (DisplayXRMcp.Dispatch).
	CHECK(shape(false, "{\"error\":\"unknown behavior 'nope'\"}") ==
	      "{\"isError\":true,\"error\":\"unknown behavior 'nope'\"}");
	// UTF-8 intact, extra members kept in order.
	CHECK(shape(false, "{\"error\":\"未知动作 😀\",\"code\":7}") ==
	      "{\"isError\":true,\"error\":\"未知动作 😀\",\"code\":7}");
	// An app that already flags it (displayxr-unity#381) is not doubled, and a
	// false flag cannot override XR_FALSE.
	CHECK(shape(false, "{\"isError\":true,\"error\":\"x\"}") == "{\"isError\":true,\"error\":\"x\"}");
	CHECK(shape(false, "{\"isError\":false,\"error\":\"x\"}") == "{\"isError\":true,\"error\":\"x\"}");
}

TEST_CASE("mcp_app_result: failure without a usable payload still says so", "[oxr][mcp]")
{
	const std::string generic = "{\"isError\":true,\"error\":\"tool failed\"}";
	CHECK(shape(false, nullptr) == generic);
	CHECK(shape(false, "") == generic);
	CHECK(shape(false, "{}") == generic); // what the runtime stores for a NULL/empty submit
	CHECK(shape(false, "\"bad input\"") == "{\"isError\":true,\"error\":\"bad input\"}");
	CHECK(shape(false, "[1]") == "{\"isError\":true,\"error\":[1]}");
	CHECK(shape(false, "boom") == "{\"isError\":true,\"error\":\"boom\"}");
}
