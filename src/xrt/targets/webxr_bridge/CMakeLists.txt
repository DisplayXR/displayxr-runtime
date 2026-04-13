# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# displayxr-webxr-bridge — tiny OpenXR client that coexists with Chrome's
# built-in WebXR session as a metadata/control sideband. Enables
# XR_EXT_display_info and XR_MND_headless, logs display info and
# rendering-mode events. Phase 1 of docs/roadmap/webxr-bridge-v2-plan.md.

if(NOT WIN32)
	return()
endif()

# The bridge is an OpenXR *client*, so it needs the OpenXR loader.
# The top-level build does not normally link the loader (the runtime
# itself is an OpenXR implementation), so we discover it here via an
# OpenXR_ROOT hint passed from scripts/build_windows.bat.
find_package(OpenXR CONFIG QUIET)
if(NOT OpenXR_FOUND)
	if(DEFINED OpenXR_ROOT AND EXISTS "${OpenXR_ROOT}/x64/lib/openxr_loader.lib")
		add_library(OpenXR::openxr_loader SHARED IMPORTED)
		set_target_properties(OpenXR::openxr_loader PROPERTIES
			IMPORTED_IMPLIB "${OpenXR_ROOT}/x64/lib/openxr_loader.lib"
			IMPORTED_LOCATION "${OpenXR_ROOT}/x64/bin/openxr_loader.dll"
		)
		set(OpenXR_FOUND TRUE)
	endif()
endif()

if(NOT OpenXR_FOUND)
	message(STATUS "webxr_bridge: OpenXR loader not found, skipping displayxr-webxr-bridge")
	return()
endif()

add_executable(displayxr-webxr-bridge main.cpp)
add_sanitizers(displayxr-webxr-bridge)

target_include_directories(displayxr-webxr-bridge PRIVATE
	${CMAKE_SOURCE_DIR}/src/external/openxr_includes
)

target_link_libraries(displayxr-webxr-bridge PRIVATE
	OpenXR::openxr_loader
	ws2_32
)

# Use static CRT for a self-contained exe, matching the other win32 targets.
if(MSVC)
	set_property(TARGET displayxr-webxr-bridge PROPERTY
		MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
	)
endif()

install(TARGETS displayxr-webxr-bridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Copy the OpenXR loader DLL next to the exe in the build tree so it can
# be run from _package/bin/ after install.
get_target_property(_loader_dll OpenXR::openxr_loader IMPORTED_LOCATION)
if(_loader_dll)
	add_custom_command(TARGET displayxr-webxr-bridge POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${_loader_dll}" "$<TARGET_FILE_DIR:displayxr-webxr-bridge>/"
		COMMENT "Copying openxr_loader.dll next to displayxr-webxr-bridge.exe"
	)
	install(FILES "${_loader_dll}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
