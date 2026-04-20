# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0

######
# displayxr-mcp — stdio ↔ per-PID-socket/pipe bridge for the MCP server.

add_executable(mcp_adapter displayxr_mcp.c)
add_sanitizers(mcp_adapter)

set_target_properties(
	mcp_adapter PROPERTIES OUTPUT_NAME displayxr-mcp PREFIX ""
	)

target_link_libraries(mcp_adapter PRIVATE aux_util)

if(WIN32)
	# pthreads via the vendored pthreads4w (same as xrt-pthreads).
	target_link_libraries(mcp_adapter PRIVATE xrt-pthreads)
endif()

install(TARGETS mcp_adapter RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
