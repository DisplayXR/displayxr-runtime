# Copyright 2019-2021, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

######
# Create the displayxr-cli diagnostics / headless self-test tool.

add_executable(
	cli
	cli_cmd_info.c
	cli_cmd_selftest.c
	cli_cmd_probe.c
	cli_cmd_test.c
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

install(TARGETS cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
