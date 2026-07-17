# Copyright 2019-2021, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

######
# Create the displayxr-cli diagnostics / headless self-test tool.

# Shared query core (cli_query_fill/run + serializers) as its own static lib
# so the Android diag bridge (targets/diag-lib, #558) can reuse it without
# pulling in the CLI executable's sources.
add_library(cli_query STATIC cli_query.c cli_query.h)
target_link_libraries(
	cli_query
	PUBLIC xrt-interfaces
	PRIVATE aux_util target_lists xrt-external-cjson
	)
target_include_directories(cli_query PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Headless zone-caps probe (#224 / ADR-027 P4): cli_query.c creates a D3D11
# WARP device on Windows to exercise the plug-in's create_dp_d3d11 +
# get_local_zone_caps without a GPU/display.
if(WIN32)
	target_link_libraries(cli_query PRIVATE d3d11)
endif()

add_executable(
	cli
	cli_cmd_info.c
	cli_cmd_selftest.c
	cli_cmd_dp.c
	cli_cmd_runtime.c
	cli_cmd_displays.c
	cli_cmd_probe.c
	cli_cmd_test.c
	cli_common.h
	cli_main.c
	)
add_sanitizers(cli)

if(WIN32)
	# Embedded Explorer icon (shared across DisplayXR executables).
	target_sources(cli PRIVATE ../common/app_icon.rc)
endif()

set_target_properties(cli PROPERTIES OUTPUT_NAME displayxr-cli PREFIX "")

target_link_libraries(
	cli
	PRIVATE
		xrt-interfaces
		aux_os
		aux_os_ble
		aux_util
		aux_math
		cli_query
		target_instance_no_comp
		target_lists
		drv_includes
	)

install(TARGETS cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
