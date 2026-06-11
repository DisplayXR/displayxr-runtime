# Copyright 2019-2021, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

######
# Create the displayxr-cli diagnostics / headless self-test tool.

add_executable(
	cli
	cli_cmd_info.c
	cli_cmd_selftest.c
	cli_cmd_dp.c
	cli_cmd_runtime.c
	cli_cmd_displays.c
	cli_cmd_probe.c
	cli_cmd_test.c
	cli_query.c
	cli_query.h
	cli_common.h
	cli_main.c
	)
add_sanitizers(cli)

set_target_properties(cli PROPERTIES OUTPUT_NAME displayxr-cli PREFIX "")

target_link_libraries(
	cli
	PRIVATE
		xrt-interfaces
		aux_os
		aux_os_ble
		aux_util
		aux_math
		target_instance_no_comp
		target_lists
		drv_includes
	)

# Headless zone-caps probe (#224 / ADR-027 P4): cli_query.c creates a D3D11
# WARP device on Windows to exercise the plug-in's create_dp_d3d11 +
# get_local_zone_caps without a GPU/display.
if(WIN32)
	target_link_libraries(cli PRIVATE d3d11)
endif()

install(TARGETS cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
