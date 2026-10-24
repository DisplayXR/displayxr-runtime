# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

# displayxr-tracking-host (ADR-054 N8, tracking-provider-discovery.md §4a):
# the process one viewer-tracking provider runs in, spawned and supervised by
# the service, writing the shared-memory viewer pose page. Windows, Linux,
# macOS. It links the trunk's tracking loader (target_lists) so the scan,
# provenance gate and ABI gate are the in-process ones.

add_executable(displayxr-tracking-host main.c)
target_link_libraries(displayxr-tracking-host PRIVATE target_lists xrt-interfaces aux_util aux_os)
# LINKER_LANGUAGE CXX: parts of aux / target_lists are C++.
set_target_properties(displayxr-tracking-host PROPERTIES LINKER_LANGUAGE CXX)

if(WIN32)
	# No console window when the service spawns it; plain main().
	set_target_properties(displayxr-tracking-host PROPERTIES WIN32_EXECUTABLE TRUE)
	if(MSVC)
		target_link_options(displayxr-tracking-host PRIVATE "/ENTRY:mainCRTStartup")
		# #1201: the shared per-monitor-v2 DPI manifest every DisplayXR executable embeds.
		target_link_options(
			displayxr-tracking-host PRIVATE "/MANIFEST:EMBED"
			"/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/../common/dpi_aware.manifest"
			)
	endif()
endif()

if(APPLE)
	# A viewer-tracking provider may open a webcam in this process (tracking-provider-discovery.md §8).
	# macOS reads the camera usage string from the executable's Info.plist; a bare executable carries
	# it as an embedded __TEXT,__info_plist section. Note: TCC asks the RESPONSIBLE process — this
	# string is what it shows once the host is its own responsible process (§10 item 1).
	set(_vhost_plist "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
	configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in ${_vhost_plist} @ONLY)
	target_link_options(displayxr-tracking-host PRIVATE "LINKER:-sectcreate,__TEXT,__info_plist,${_vhost_plist}")
	set_property(TARGET displayxr-tracking-host APPEND PROPERTY LINK_DEPENDS ${_vhost_plist})
endif()

# In the build tree, sit next to the runtime library: providers built against
# it import the aux surface from it (macOS: @rpath/openxr_displayxr.dylib,
# resolved through the host's rpath; the staged / installed host gets
# @executable_path/../lib from build_macos.sh), exactly like the conversion host.
if(TARGET ${RUNTIME_TARGET} AND (WIN32 OR APPLE))
	set_target_properties(
		displayxr-tracking-host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:${RUNTIME_TARGET}>"
		)
	if(APPLE)
		set_property(TARGET displayxr-tracking-host APPEND PROPERTY BUILD_RPATH "@executable_path")
	endif()
endif()

install(TARGETS displayxr-tracking-host RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
