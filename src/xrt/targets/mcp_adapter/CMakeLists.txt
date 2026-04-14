# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0

######
# displayxr-mcp — stdio ↔ per-PID-socket bridge for the MCP server.
# POSIX-only in Phase A slice 1–6; Windows named-pipe path lands in slice 7.

if(WIN32)
	return()
endif()

add_executable(mcp_adapter displayxr_mcp.c)
add_sanitizers(mcp_adapter)

set_target_properties(
	mcp_adapter PROPERTIES OUTPUT_NAME displayxr-mcp PREFIX ""
	)

target_link_libraries(mcp_adapter PRIVATE aux_util)

install(TARGETS mcp_adapter RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
