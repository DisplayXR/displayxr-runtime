# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

# displayxr-conversion-host (ADR-053 D5, #2033): the satellite process one
# conversion provider runs in, supervised by the service. One binary for every
# provider; Windows, Linux and macOS (CPU lane everywhere, the D3D11 lane on
# Windows).
#
# conv_host_core: everything but main(), so tests can reuse pure pieces
# (argument parsing, the wire framing over os_channel) without spawning. The
# channel, wake, parent-watch and elevation primitives are aux_os's
# os_process.h (#2032).

add_library(
	conv_host_core STATIC
	conv_host_args.c
	conv_host_args.h
	conv_host_chan.c
	conv_host_chan.h
	conv_host_exit.h
	conv_host_log.c
	conv_host_log.h
	conv_host_os.h
	conv_host_provider.c
	conv_host_provider.h
	conv_host_serve.c
	conv_host_serve.h
	conv_host_shm.c
	conv_host_shm.h
	)
target_include_directories(conv_host_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
	conv_host_core
	PUBLIC xrt-interfaces aux_util aux_os displayxr::conversion_runtime target_lists
	)
if(WIN32)
	target_sources(conv_host_core PRIVATE conv_host_os_win.c conv_host_d3d11.cpp conv_host_d3d11.h)
	target_link_libraries(conv_host_core PUBLIC d3d11 dxgi shell32 user32 advapi32)
else()
	target_sources(conv_host_core PRIVATE conv_host_os_posix.c)
	target_link_libraries(conv_host_core PUBLIC ${CMAKE_DL_LIBS})
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		# shm_open lives in librt on glibc < 2.34 (Ubuntu 22.04 is 2.35, but
		# keep the floor explicit for older toolchains).
		target_link_libraries(conv_host_core PUBLIC rt xrt-pthreads)
	endif()
endif()

add_executable(displayxr-conversion-host main.c)
target_link_libraries(displayxr-conversion-host PRIVATE conv_host_core)
# LINKER_LANGUAGE CXX: the D3D11 lane and parts of aux are C++.
set_target_properties(displayxr-conversion-host PROPERTIES LINKER_LANGUAGE CXX)

if(WIN32)
	# No console window when the supervisor spawns it; plain main().
	set_target_properties(displayxr-conversion-host PROPERTIES WIN32_EXECUTABLE TRUE)
	if(MSVC)
		target_link_options(displayxr-conversion-host PRIVATE "/ENTRY:mainCRTStartup")
		# #1201: the shared per-monitor-v2 DPI manifest every DisplayXR
		# executable embeds (the host creates no visible window, but a
		# provider dependency reading geometry must not see virtualised DPI).
		target_link_options(
			displayxr-conversion-host PRIVATE "/MANIFEST:EMBED"
			"/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/../common/dpi_aware.manifest"
			)
	endif()
endif()

# In the build tree, sit next to the runtime library: providers built with
# XRT_USING_RUNTIME_DLL import the aux surface from it, and the conversion
# loader pre-loads it from the host's own directory (the installed layout:
# both in bin/).
if(TARGET ${RUNTIME_TARGET} AND (WIN32 OR APPLE))
	set_target_properties(
		displayxr-conversion-host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:${RUNTIME_TARGET}>"
		)
endif()

install(TARGETS displayxr-conversion-host RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
