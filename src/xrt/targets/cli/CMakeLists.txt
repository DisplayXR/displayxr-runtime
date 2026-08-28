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

	# #918 GPU-topology section: DXGI adapter enumeration (dxgi + dxguid for
	# IID_IDXGIFactory1 in C) plus aux_d3d's panel-rect -> scanout-adapter
	# resolver. Same guarded link targets/common uses for the same resolver.
	target_link_libraries(cli_query PRIVATE dxgi dxguid)
	if(TARGET aux_d3d)
		target_link_libraries(cli_query PRIVATE aux_d3d)
	endif()
endif()

add_executable(
	cli
	cli_cmd_info.c
	cli_cmd_selftest.c
	cli_cmd_dp.c
	cli_cmd_perf.c
	cli_cmd_input.c
	cli_cmd_runtime.c
	cli_cmd_displays.c
	cli_cmd_probe.c
	cli_cmd_test.c
	cli_cmd_clients.c
	cli_common.h
	cli_main.c
	)
add_sanitizers(cli)

if(WIN32)
	# Embedded Explorer icon (shared across DisplayXR executables).
	target_sources(cli PRIVATE ../common/app_icon.rc)

	# #1201 — embed the shared per-monitor-v2 DPI manifest (merged with the
	# default UAC manifest). Without it this process is handed VIRTUALISED
	# coordinates, so on a scaled panel `info`/`displays` report the logical
	# resolution (2560x1440 on a 4K panel at 150%) and `selftest` asserts it.
	# cli_main.c also calls u_win_make_process_dpi_aware() as the backstop for
	# toolchains that cannot embed a manifest.
	if(MSVC)
		target_link_options(
			cli PRIVATE "/MANIFEST:EMBED"
			"/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/../common/dpi_aware.manifest"
			)
	endif()
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

# #960: `displayxr-cli clients` connects to the live service over IPC (DIAG class).
# Same gating as targets/diag-lib.
if(XRT_FEATURE_SERVICE AND XRT_MODULE_IPC)
	target_link_libraries(cli PRIVATE ipc_client)
	target_compile_definitions(cli PRIVATE CLI_HAVE_IPC)
endif()

install(TARGETS cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
